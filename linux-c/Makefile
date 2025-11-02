iterative: 01_iterative/main.c
	gcc -o $@ $<

forking: 02_forking/main.c
	gcc -o $@ $<

preforked: 03_preforked/main.c
	gcc -o $@ $<

all: iterative

.PHONY: clean

clean:
	rm -f iterative forking preforked