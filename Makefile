CFLAGS = -Wall -std=gnu99

.PHONY: all clean check

-include config.mak

HOSTCC ?= $(CC)

SYNTAX_NANORC := $(wildcard syntax/*.nanorc)
SYNTAX_HEADERS := $(patsubst syntax/%.nanorc,syntax/%.h,$(SYNTAX_NANORC))

all: nanorc.h culo nanorc2h

culo: culo.c nregex.c
	$(CC) $(CFLAGS) -o $@ culo.c nregex.c $(LDFLAGS)

nanorc2h: nanorc2h.c
	$(HOSTCC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

syntax/%.h: syntax/%.nanorc nanorc2h syntax.h
	./nanorc2h $< > $@

nanorc.h: $(SYNTAX_HEADERS)
	$(RM) $@
	@for f in $(SYNTAX_HEADERS); do echo "#include \"$$f\"" >> $@; done

culo: nanorc.h syntax.h

check: culo
	@tests/runner.sh

clean:
	$(RM) culo
	$(RM) nanorc2h
	$(RM) nanorc.h
	$(RM) $(SYNTAX_HEADERS)
	$(RM) -rf tests/test_tmp
	$(RM) -f tests/*.txt tests/*.log
