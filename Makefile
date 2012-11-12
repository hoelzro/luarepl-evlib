CFLAGS+=-fPIC

luarepl-ev.so: luarepl-ev.o
	gcc -o $@ $^ -shared -lev -llua -lm -ldl

clean:
	rm -f *.so
