# luarepl-evlib - Use luarepl ina a program that's already using an event loop

Some programs already use Lua as a scripting language, and also use an event loop.
By linking against the shared library that this project produces and invoking a function,
such a program can add a luarepl-powered REPL accessible over a TCP socket.

See also: https://github.com/hoelzro/lua-repl

# Limitations

The current implementation is written to work with libev.  It shouldn't be too hard to break out
the libev-specific logic, but it still remains to be done.

Also, the current implementation will work only with a TCP socket.  I don't see a problem with
working with UNIX sockets, or even a terminal.  Because all of the I/O is done via `read()` and
`write()`, this should be another "easy" task; it simply remains to be done.
