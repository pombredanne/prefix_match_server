CC=g++
RM=rm
INCLUDES=-I./ -I/usr/local/event/include
CPPFLAGS+=-O2 ${INCLUDES} -fPIC -g --std=c++0x

LINKFLAGS+=-L./ -L/usr/local/event/lib/
LIBS=-levent -lpthread -lm -rdynamic  

SERVEROBJS=config.o conn.o sig.o log.o prefixmatch.o thread.o network.o util.o server.o
CLIENTOBJS=client.o

all:server client
install:server client

OBJS1=$(SERVEROBJS) 

server:$(OBJS1)
	$(CC) $(OBJS1) $(LINKFLAGS) $(LIBS) -o $@

client:$(CLIENTOBJS)
	$(CC) $(CLIENTOBJS) $(LINKFLAGS) $(LIBS) -o $@

%.o:%.c %.cpp %.h
	$(CC) -c $(CPPFLAGS) $< -o $@

clean:
	-$(RM) -f *.o server client
