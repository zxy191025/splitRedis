CC = gcc
CFLAGS = -Wall -o2 -Iinclude
.PHONY:clean

app:util.o

util.o:util.c
	$(CC) $(CFLAGS) -c util.c -o util.o

clean:
	rm -f *.o 
