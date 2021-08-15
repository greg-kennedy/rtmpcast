CFLAGS += -Wall -Wextra
IFLAGS += -I/usr/local/include
LFLAGS += -L/usr/local/lib

all:	rtmpcast

rtmpcast:	main.c
	cc $(IFLAGS) $(CFLAGS) $(LFLAGS) -o rtmpcast main.c -lrtmp

clean:
	rm -f rtmpcast *.o
