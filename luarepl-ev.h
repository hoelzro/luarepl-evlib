#ifndef LUAREPL_EV_H
#define LUAREPL_EV_H

#include <ev.h>
#include <lua.h>
#include <stdint.h>

int luarepl_ev_start(lua_State *L, struct ev_loop *loop, const char *bind_address, uint16_t port);

/* XXX I should probably include a stop call */

#endif
