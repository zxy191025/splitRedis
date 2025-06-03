CC = gcc
CFLAGS = -Wall -o0 -g
app:zmalloc.o intset.o

zmalloc.o:zmalloc.c
	$(CC) $(CFLAGS) -c zmalloc.c -o zmalloc.o

intset.o:intset.c
	$(CC) $(CFLAGS) -c intset.c -o intset.o
