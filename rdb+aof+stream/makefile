CC = gcc
CFLAGS = -Wall -o2 -Iinclude
.PHONY:clean

app:rdb.o stream.o aof.o

rdb.o:rdb.c
	$(CC) $(CFLAGS) -c rdb.c -o rdb.o

stream.o:t_stream.c
	$(CC) $(CFLAGS) -c t_stream.c -o stream.o

aof.o:aof.c
	$(CC) $(CFLAGS) -c aof.c -o aof.o

clean:
	rm -f *.o 