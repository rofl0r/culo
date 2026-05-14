CFLAGS = -Wall -std=gnu99

.PHONY: all clean check

-include config.mak

SYNTAX_NANORC := $(wildcard syntax/*.nanorc)
SYNTAX_HEADERS := $(patsubst syntax/%.nanorc,syntax/%.h,$(SYNTAX_NANORC))

all: nanorc.h culo nanorc2h

me: culo.c nregex.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

nanorc2h: nanorc2h.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

syntax/%.h: syntax/%.nanorc nanorc2h syntax.h
	./nanorc2h $< > $@

nanorc.h: $(SYNTAX_HEADERS)
	$(RM) $@
	@for f in $(SYNTAX_HEADERS); do echo "#include \"$$f\"" >> $@; done

culo: nanorc.h syntax.h
culo: culo.c
	$(CC) $(CFLAGS) -o $@ culo.c $(LDFLAGS)

check: culo
	@tests/runner.sh

clean:
	$(RM) me
	$(RM) nanorc2h
	$(RM) nanorc.h
	$(RM) $(SYNTAX_HEADERS)
	$(RM) -rf tests/test_tmp
	$(RM) -f tests/*.txt tests/*.log
