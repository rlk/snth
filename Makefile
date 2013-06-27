
CFLAGS= -Wall -g -msse
CC= gcc
RM= rm

TARG= snthgui
OBJS= snth.o gui.o
LIBS= -lasound

GTK_OPTS= \
	$(shell pkg-config --cflags gtk+-2.0) \
	$(shell pkg-config --cflags gthread-2.0)
GTK_LIBS= \
	$(shell pkg-config --libs gtk+-2.0) \
	$(shell pkg-config --libs gthread-2.0)

#------------------------------------------------------------------------------

.c.o :
	$(CC) $(CFLAGS) $(GTK_OPTS) -c $<

$(TARG) : $(OBJS)
	$(CC) $(CFLAGS) $(GTK_OPTS) -o $(TARG) $(OBJS) $(GTK_LIBS) $(LIBS)

clean :
	$(RM) -f $(TARG) $(OBJS)

#------------------------------------------------------------------------------

snth.o : snth.h Makefile
gui.o  : snth.h Makefile
