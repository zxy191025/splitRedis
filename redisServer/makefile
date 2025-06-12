CC = gcc
CFLAGS = -Wall -o2 -Iinclude
.PHONY:clean

app:server.o 

server.o:server.c
	$(CC) $(CFLAGS) -c server.c -o server.o

clean:
	rm -f *.o 