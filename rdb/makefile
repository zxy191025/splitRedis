CC = gcc
CFLAGS = -Wall -o2 -Iinclude
.PHONY:clean

app:rdb.o stream.o

rdb.o:rdb.c
	$(CC) $(CFLAGS) -c rdb.c -o rdb.o

stream.o:t_stream.c
	$(CC) $(CFLAGS) -c t_stream.c -o stream.o
clean:
	rm -f *.o 