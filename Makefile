CFLAGS = -Wall -std=gnu99

.PHONY: all clean check

-include config.mak

all: culo

me: culo.c nregex.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

check: culo
	@tests/runner.sh

clean:
	$(RM) me
	$(RM) -rf tests/test_tmp
	$(RM) -f tests/*.txt tests/*.log
