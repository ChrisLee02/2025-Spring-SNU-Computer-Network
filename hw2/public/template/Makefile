all: shttpd

CFLAGS = -Wall -Werror

shttpd: shttpd.c macro.h
	gcc ${CFLAGS} -o shttpd shttpd.c

clean:
	rm shttpd
