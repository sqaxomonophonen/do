USE=-DUSE_GLX11
OPT=-g -O0
STD=-std=gnu99
CFLAGS=$(OPT) $(STD) -Wall $(USE)
LINK=-lm -lX11 -lGL -lrt -Wall

BIN=do

all: $(BIN)

m.o: m.c
	$(CC) $(CFLAGS) -c $<

atls.o: atls.c
	$(CC) $(CFLAGS) -c $<

lsl_prg.o: lsl_prg.c
	$(CC) $(CFLAGS) -c $<

dya.o: dya.c
	$(CC) $(CFLAGS) -c $<

dd.o: dd.c
	$(CC) $(CFLAGS) -c $<

do.o: do.c
	$(CC) $(CFLAGS) -c $<

do: do.o dd.o dya.o lsl_prg.o m.o atls.o
	$(CC) $(LINK) $^ -o $@

clean:
	rm -f *.o $(BIN)


