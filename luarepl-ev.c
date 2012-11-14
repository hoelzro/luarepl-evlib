/* Copyright (c) 2012 Rob Hoelz <rob@hoelz.ro>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "luarepl-ev.h"
#include <lauxlib.h>

#define LISTEN_BACKLOG_SIZE 5
#define STATIC_SPACE_SIZE 256

struct lua_server_io {
    ev_io w;

    lua_State *L;
    int repl_ref;
};

struct lua_client_io {
    ev_io w;

    lua_State *L;
    int repl_ref;

    char static_space[STATIC_SPACE_SIZE];
    char *buffer;
    size_t buffer_size;
    size_t buffer_offset;
};

static void
luaR_dumpstack(lua_State *L)
{
    int i;
    for(i = 1; i <= lua_gettop(L); i++) {
        printf("Lua(%d) - %s\n", i, lua_typename(L, lua_type(L, i)));
    };
}

static void
luaR_callmethod(lua_State *L, const char *method, int nargs, int nret)
{
    /* create a copy of self below its index on the stack */
    lua_pushvalue(L, -1 - nargs);
    lua_insert(L, -1 - nargs);

    /* retrieves the method corresponding to the name and inserts it
     * into place */
    lua_getfield(L, -1 - nargs, method);
    lua_insert(L, -1 - nargs - 1);

    lua_call(L, nargs + 1, nret);
}

static const char *
luaR_tostring(lua_State *L, int index)
{
    const char *string_value;

    lua_pushvalue(L, index); // value
    lua_getglobal(L, "tostring"); // value tostring
    lua_insert(L, -2); // tostring value
    lua_call(L, 1, 1); // tostring(value);

    return lua_tostring(L, -1);
}

static int
setup_server_socket(lua_State *L, const char *bind_addr, uint16_t port)
{
    int status;
    int sock;
    struct sockaddr_in address;

    address.sin_family = AF_INET;
    address.sin_port   = htons(port);
    status = inet_aton(bind_addr, &address.sin_addr);

    if(status == 0) {
        lua_pushfstring(L, "Unable to parse address: %s", strerror(errno));
        return 0;
    }

    sock = socket(AF_INET, SOCK_STREAM, getprotobyname("tcp")->p_proto);
    if(sock < 0) {
        lua_pushfstring(L, "Unable to create socket: %s", strerror(errno));
        return 0;
    }

    status = bind(sock, (struct sockaddr *) &address, sizeof(address));
    if(status < 0) {
        lua_pushfstring(L, "Unable to bind: %s", strerror(errno));
        return 0;
    }

    status = listen(sock, LISTEN_BACKLOG_SIZE);
    if(status < 0) {
        lua_pushfstring(L, "Unable to listen: %s", strerror(errno));
        return 0;
    }

    return sock;
}

static void
process_line(struct lua_client_io *lua_w, const char *line)
{
    lua_State *L = lua_w->L;

    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_w->repl_ref);
    lua_pushstring(L, line);
    luaR_callmethod(L, "handleline", 1, 1);
    luaR_callmethod(L, "prompt", 1, 0);
    lua_pop(L, 1); /* pop repl object */
}

static void
process_lines(struct lua_client_io *lua_w)
{
    char *p = lua_w->buffer;
    char *nl;

    while(nl = strchr(p, '\n')) {
        *nl = '\0';
        process_line(lua_w, p);
        p = nl + 1;
    }

    memmove(lua_w->buffer, p, lua_w->buffer_offset - (p - lua_w->buffer));
    lua_w->buffer_offset -= p - lua_w->buffer;
}

static void
client_sock_cb(EV_P_ ev_io *w, int revents)
{
    struct lua_client_io *lua_w = (struct lua_client_io *) w;
    int fd               = w->fd;
    ssize_t bytes_read;

    bytes_read = read(fd, lua_w->buffer + lua_w->buffer_offset,
        lua_w->buffer_size - 1 - lua_w->buffer_offset);

    if(bytes_read <= 0) {
        close(fd);
        ev_io_stop(EV_A_ w);
        if(lua_w->buffer != lua_w->static_space) {
            free(lua_w->buffer);
        }
        lua_pushnil(lua_w->L);
        lua_rawseti(lua_w->L, LUA_REGISTRYINDEX, lua_w->repl_ref);
        free(lua_w);
    } else {
        lua_w->buffer[lua_w->buffer_offset + bytes_read] = '\0';
        lua_w->buffer_offset += bytes_read;
        process_lines(lua_w);

        if(lua_w->buffer_size == lua_w->buffer_offset + 1) {
            char *old_buffer    = lua_w->buffer;
            size_t old_size     = lua_w->buffer_size;
            lua_w->buffer_size *= 2;
            lua_w->buffer       = malloc(lua_w->buffer_size);

            if(! lua_w->buffer) {
                /* ruh-roh */
            }

            memcpy(lua_w->buffer, old_buffer, old_size);

            if(old_buffer != lua_w->static_space) {
                free(old_buffer);
            }
        }

        if(lua_w->buffer_offset < STATIC_SPACE_SIZE - 1 && lua_w->buffer != lua_w->static_space) {
            memcpy(lua_w->static_space, lua_w->buffer, lua_w->buffer_offset);
            free(lua_w->buffer);
            lua_w->buffer      = lua_w->static_space;
            lua_w->buffer_size = STATIC_SPACE_SIZE;
        }
    }
}

static void
server_sock_cb(EV_P_ ev_io *w, int revents)
{
    struct lua_server_io *lua_server = (struct lua_server_io *) w;
    int server_sock                  = w->fd;
    int client_sock;

    client_sock = accept(server_sock, NULL, NULL);
    if(client_sock > 0) {
        struct lua_client_io *client_sock_w;

        client_sock_w = malloc(sizeof(struct lua_client_io));
        if(! client_sock_w) {
            /* XXX log it */
            close(client_sock);
            return;
        }

        lua_rawgeti(lua_server->L, LUA_REGISTRYINDEX, lua_server->repl_ref);
        luaR_callmethod(lua_server->L, "clone", 0, 1);
        lua_pushinteger(lua_server->L, client_sock);
        lua_setfield(lua_server->L, -2, "_fd");

        /* I should probably set the FD to non-blocking */
        ev_io_init((ev_io *) client_sock_w, client_sock_cb, client_sock, EV_READ);
        client_sock_w->L             = lua_server->L;
        client_sock_w->repl_ref      = luaL_ref(lua_server->L, LUA_REGISTRYINDEX);
        client_sock_w->buffer        = client_sock_w->static_space;
        client_sock_w->buffer_size   = STATIC_SPACE_SIZE;
        client_sock_w->buffer_offset = 0;
        ev_io_start(EV_A_ (ev_io *) client_sock_w);

        lua_pop(lua_server->L, 1); /* pop repl clone from server */
    } else {
        /* ruh-roh */
        /* I should probably re-initialize the watcher, or kill the REPL */
        /* if I close up shop, I should stop the event watcher, and remove
         * the Lua registry reference for this watcher */
    }
}

static struct lua_server_io server_sock_w;

static int
luarepl_ev_send(lua_State *L)
{
    int fd;
    const char *string;
    size_t length;

    lua_getfield(L, 1, "_fd");
    fd = lua_tointeger(L, -1);
    lua_pop(L, 1);
    string = lua_tolstring(L, 2, &length);
    /* XXX make sure write is OK */
    write(fd, string, length);

    return 0;
}

static int
luarepl_ev_displayresults(lua_State *L)
{
    int i;
    int n;

    lua_settop(L, 2);
    lua_getfield(L, 2, "n");
    n = lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_checkstack(L, 2 * n + 1);

    if(n) {
        for(i = 1; i <= n; i++) {
            lua_rawgeti(L, 2, i);
            luaR_tostring(L, -1);
            lua_remove(L, -2);

            if(i != n) {
                lua_pushliteral(L, "\t");
            }
        }

        lua_concat(L, 2 * n - 1);
        lua_pushvalue(L, 1);
        lua_insert(L, -2);
        luaR_callmethod(L, "send", 1, 0);
    }
    return 0;
}

static void
add_repl_methods(lua_State *L)
{
    luaL_Reg methods[] = {
        { "displayresults", luarepl_ev_displayresults },
        { "displayerror", luarepl_ev_send },
        { "showprompt", luarepl_ev_send },
        { "send", luarepl_ev_send },
        { NULL, NULL }
    };

    luaL_register(L, NULL, methods);
}

int
luarepl_ev_start(lua_State *L, struct ev_loop *loop, const char *bind_addr, uint16_t port)
{
    int status;
    int server_sock;

    lua_getglobal(L, "require");
    lua_pushliteral(L, "repl");
    status = lua_pcall(L, 1, 1, 0);

    if(status) {
        return status;
    }

    luaR_callmethod(L, "clone", 0, 1);
    add_repl_methods(L);

    server_sock = setup_server_socket(L, bind_addr, port);
    if(! server_sock) {
        return 1;
    }
    ev_io_init((ev_io *) &server_sock_w, server_sock_cb, server_sock, EV_READ);
    server_sock_w.L        = L;
    server_sock_w.repl_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ev_io_start(loop, (ev_io *) &server_sock_w);

    lua_pop(L, 1); /* pop the result of require 'repl' */

    return 0;
}

/* TODO
 *
 *   - Make the interface less specific to libev
 *   - Function naming conventions?
 *   - Implement "advanced client" mode (detected with high byte or something)
 *   - Logging
 *   - Split up client_sock_cb
 *   - Handle ruh-roh's
 *   - Implement "advanced client"
 */
