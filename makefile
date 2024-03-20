CFLAGS= -Wall -pedantic -std=gnu99 -g
NETFLEX_PATH=./netflex
NETFLEX_LIB= -I $(NETFLEX_PATH)

all: libflex flexhq

libflex: $(NETFLEX_PATH)/libflex.c
	gcc -c $(CFLAGS) $(NETFLEX_LIB) $(NETFLEX_PATH)/libflex.c -o $(NETFLEX_PATH)/libflex.o

flexhq: flexhq.c
	gcc $(CFLAGS) $(NETFLEX_LIB) flexhq.c $(NETFLEX_PATH)/libflex.o -o flexhqd

