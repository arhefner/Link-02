PROJECT=link02
CC=gcc
DEFS=-std=c99
INDIR=
INCS=
LIBDIR=
LIBS=
OBJS=\
	main.o

$(PROJECT): $(OBJS)
	$(CC) $(DEFS) $(LIBDIR) $(OBJS) $(LIBS) -o $(PROJECT)

.c.o:
	$(CC) $(DEFS) $(INCDIR) $(INCS) -c $<

clean:
	-rm *.o
	-rm $(PROJECT)

install:
	cp link02 /usr/local/bin
	chmod a+rx /usr/local/bin/link02

main.o:                header.h main.c

