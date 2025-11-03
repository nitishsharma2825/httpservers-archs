iterative: 01_iterative/main.c
	gcc -o $@ $<

forking: 02_forking/main.c
	gcc -o $@ $<

preforked: 03_preforked/main.c
	gcc -o $@ $<

threaded: 04_threaded/main.c
	gcc -o $@ $<

prethreaded: 05_prethreaded/main.c
	gcc -o $@ $<

all: iterative forked preforked threaded prethreaded

.PHONY: clean

clean:
	rm -f iterative forking preforked threaded prethreaded