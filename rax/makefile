CC = gcc
CFLAGS = -Wall -o2 -Iinclude
.PHONY:clean

app:rax.o 

rax.o:rax.c
	$(CC) $(CFLAGS) -c rax.c -o rax.o

clean:
	rm -f *.o 