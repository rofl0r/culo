CFLAGS = -Wall -std=gnu99

.PHONY: all clean check

-include config.mak
all: me

me: me.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

check: me
	@tests/runner.sh

clean:
	$(RM) me
	$(RM) -rf tests/test_tmp
	$(RM) -f tests/*.txt tests/*.log
