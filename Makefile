CC=gcc
CFLAGS=-Wall -Wextra -O2 -pthread

all: client ns ds

client: client.c protocol.c
	$(CC) $(CFLAGS) -o client client.c protocol.c

ns: fileNameServer.c protocol.c
	$(CC) $(CFLAGS) -o ns fileNameServer.c protocol.c

ds: fileDataServer.c protocol.c
	$(CC) $(CFLAGS) -o ds fileDataServer.c protocol.c

clean:
	rm -f client ns ds
	rm -rf fhm.root storage_*

.PHONY: all clean
