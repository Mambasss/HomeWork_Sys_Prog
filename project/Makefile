CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -g -D_GNU_SOURCE -Iinclude
LDFLAGS :=
BIN     := bin

all: $(BIN)/minidockerd $(BIN)/minidocker

$(BIN):
	mkdir -p $(BIN)

$(BIN)/minidockerd: src/daemon.c src/container.c src/util.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/minidocker: src/client.c src/util.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf $(BIN)

.PHONY: all clean