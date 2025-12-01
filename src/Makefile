EXECUTABLES = csvtbin dumptap readtape 
READTAPE_OBJS = decoder.o readtape.o parmsets.o decode_ww.o \
				textfile.o decode_nrzi.o decode_pe.o \
				decode_gcr.o tapread.o ibmlabels.o trace.o

CFLAGS += --std=c99 -Wall -Wno-unused-variable -Wno-unused-but-set-variable -D_DEFAULT_SOURCE
LDFLAGS += -lm

.PHONY: all clean

all: $(EXECUTABLES)

dumptap: dumptap.o
	$(CC) $(LDFLAGS) -o $@ $^
csvtbin: csvtbin.o
	$(CC) $(LDFLAGS) -o $@ $^
readtape: $(READTAPE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

clean:
	rm -f $(EXECUTABLES) $(READTAPE_OBJS) dumptap.o csvtbin.o
