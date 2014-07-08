CC=g++
RM=rm
INCLUDES=-I./
CPPFLAGS+=-O2 ${INCLUDES} -fPIC -g --std=c++0x

LINKFLAGS+=-L./ 
LIBS=-lpthread -lm -rdynamic  

BASEOBJS=util.o indexer.o 

all:indexer

OBJS=$(BASEOBJS) 
indexer:$(OBJS)
	$(CC) $(OBJS) $(LINKFLAGS) $(LIBS) -o $@
%.o:%.c %.cpp %.h
	$(CC) -c $(CPPFLAGS) $< -o $@
clean:
	-$(RM) -f *.o indexerer
