CC ?= cc
CFLAGS ?= -O2 -g
WARN = -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow
PREFIX ?= /usr/local
all: fenced-run
fenced-run: src/fenced-run.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) $< -o $@
tests/no-pidfd: tests/no-pidfd.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) $< -o $@
fenced-run-procscan: src/fenced-run.c tests/no-task-children.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) $^ -Wl,--wrap=open -o $@
test: fenced-run fenced-run-procscan tests/no-pidfd
	python3 tests/test_processes.py ./fenced-run
	python3 tests/test_processes.py ./fenced-run-procscan
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
install: fenced-run
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 fenced-run $(DESTDIR)$(PREFIX)/bin/fenced-run
clean:
	rm -f fenced-run fenced-run-procscan tests/no-pidfd
.PHONY: all test sanitize install clean

integration: fenced-run
	tests/test_network.sh ./fenced-run
openwrt-package:
	mkdir -p build/openwrt/fenced-run/src
	cp packaging/openwrt/Makefile COPYING build/openwrt/fenced-run/
	cp src/fenced-run.c build/openwrt/fenced-run/src/
.PHONY: integration openwrt-package
