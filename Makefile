CFLAGS = -Wall -W -Wp,-D_FORTIFY_SOURCE=2 -O2 -std=gnu99

all:		fbtest

clean:
	rm -f fbtest *.o
