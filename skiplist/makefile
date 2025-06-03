CC = gcc
CFLAGS = -Wall -o0 -g
app: skiplist.o zmalloc.o

zmalloc.o:zmalloc.c
	$(CC) $(CFLAGS) -c zmalloc.c -o zmalloc.o
	
skiplist.o:skiplist.c
	$(CC) $(CFLAGS) -c skiplist.c -o skiplist.o
