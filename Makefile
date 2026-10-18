CFLAGS  = -std=c99 -Wall -Wextra -O2 $(shell pkg-config --cflags raylib)
LDLIBS  = $(shell pkg-config --libs raylib) -lm

bin/dfa: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

run: bin/dfa
	./bin/dfa examples/ends-abb.dot

clean:
	rm -f dfa

.PHONY: run clean
