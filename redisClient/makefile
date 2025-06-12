CC = gcc
CFLAGS = -Wall -o2 -Iinclude

app:redis-cli.o

redis-cli.o:redis-cli.c
	$(CC) $(CFLAGS) -c redis-cli.c -o redis-cli.o


.PHONY:clean

clean:
	rm -f *.o 