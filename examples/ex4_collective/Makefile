include ../opts.mk

all: put get

get: get.o
	$(CC) -o $@ $< $(LDFLAGS)

put: put.o
	$(CC) -o $@ $< $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f get put *.o
