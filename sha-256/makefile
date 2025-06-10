CC = gcc
CFLAGS = -Wall -o2 -Iinclude
.PHONY:clean

app:sha256.o

sha256.o:sha256.c
	$(CC) $(CFLAGS) -c sha256.c -o sha256.o

clean:
	rm -f *.o 
