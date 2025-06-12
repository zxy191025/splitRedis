CC = gcc
CFLAGS = -Wall -o2 -Iinclude

app:anet.o ae.o

anet.o:anet.c
	$(CC) $(CFLAGS) -c anet.c -o anet.o

ae.o:ae.c
	$(CC) $(CFLAGS) -c ae.c -o ae.o

.PHONY:clean

clean:
	rm -f *.o 