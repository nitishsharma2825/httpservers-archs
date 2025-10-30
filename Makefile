iterative: 01_iterative/main.c
	gcc -o $@ $<

all: iterative

.PHONY: clean

clean:
	rm -f iterative