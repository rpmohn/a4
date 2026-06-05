.POSIX:
.PRAGMA: posix_202x
.SUFFIXES:
.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<
.PHONY: all debug portable portable-arch release clean distclean install uninstall

PREFIX		= /usr/local
MANPREFIX	= $(PREFIX)/share/man
SYSCONFDIR	= $(PREFIX)/share
DOCDIR		= $(PREFIX)/share/doc

VERSION		!= git describe --always --dirty 2>/dev/null || echo "v2.0"

CPPFLAGS	= -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_XOPEN_SOURCE_EXTENDED \
			  -DNDEBUG -DSYSCONFDIR='"$(SYSCONFDIR)"' $(CPPFLGS) -DVERSION='"$(VERSION)"' \
			  $(unibilium_flags) $(termkey_flags) $(tickit_flags) $(vterm_flags)
CFLAGS		= -std=c99 -Wall
DEBUG		= -UNDEBUG -O0 -g -ggdb -Wextra -Wno-unused-parameter -fdiagnostics-color=always
LDLIBS		= -lutil

a4_obj		= a4.o session.o
inih_obj	= lib/inih/ini.o
uni_obj		= lib/unibilium/unibilium.o lib/unibilium/uninames.o lib/unibilium/uniutil.o
termkey_obj	= lib/libtermkey/termkey.o lib/libtermkey/driver-csi.o lib/libtermkey/driver-ti.o
tickit_obj	= lib/libtickit/src/bindings.o lib/libtickit/src/debug.o lib/libtickit/src/evloop-default.o \
			  lib/libtickit/src/pen.o lib/libtickit/src/rect.o lib/libtickit/src/rectset.o \
			  lib/libtickit/src/renderbuffer.o lib/libtickit/src/string.o lib/libtickit/src/term.o \
			  lib/libtickit/src/termdriver-ti.o lib/libtickit/src/termdriver-xterm.o \
			  lib/libtickit/src/tickit.o lib/libtickit/src/utf8.o lib/libtickit/src/window.o
vterm_obj	= lib/libvterm/src/encoding.o lib/libvterm/src/keyboard.o lib/libvterm/src/mouse.o \
			  lib/libvterm/src/parser.o lib/libvterm/src/pen.o lib/libvterm/src/screen.o \
			  lib/libvterm/src/state.o lib/libvterm/src/unicode.o lib/libvterm/src/vterm.o
obj			= $(a4_obj) $(inih_obj) $(uni_obj) $(termkey_obj) $(tickit_obj) $(vterm_obj)

all: a4 extras/a4-keycodes

a4: $(obj)
	$(CC) $(LDFLAGS) -o $@ $(obj) $(LDLIBS)

a4.o: config.c layouts.c utilities.c vt.c lib/keynames.inc lib/rgb.inc lib/libvterm/src/utf8.h session.h

session.o: session.h

extras/a4-keycodes: extras/a4-keycodes.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $? $(LDFLAGS) $(uni_obj) $(termkey_obj) $(tickit_obj) -o $@

debug: clean
	@$(MAKE) CFLAGS='$(CFLAGS) $(DEBUG)' a4

portable: distclean
	@$(MAKE) portable-arch ARCH=x86_64 ZIG_TARGET=x86_64-linux-gnu.2.17
	@$(MAKE) portable-arch ARCH=arm64 ZIG_TARGET=aarch64-linux-gnu.2.17
	@$(MAKE) portable-arch ARCH=armv7 ZIG_TARGET=arm-linux-gnueabihf.2.17

portable-musl: distclean
	@$(MAKE) portable-arch ARCH=x86_64-musl ZIG_TARGET=x86_64-linux-musl LDLIBS=
	@$(MAKE) portable-arch ARCH=arm64-musl ZIG_TARGET=aarch64-linux-musl LDLIBS=
	@$(MAKE) portable-arch ARCH=armv7-musl ZIG_TARGET=arm-linux-musleabihf LDLIBS=

portable-arch:
	@$(MAKE) CC='zig cc -target $(ZIG_TARGET)' LDLIBS='$(LDLIBS)' a4
	mv a4 a4-$(ARCH)
	@$(MAKE) CC='zig cc -target $(ZIG_TARGET)' LDLIBS='$(LDLIBS)' extras/a4-keycodes
	mv extras/a4-keycodes extras/a4-keycodes-$(ARCH)
	rm -f $(obj)

release: portable portable-musl
	rm -rf release && mkdir -p release
	for arch in x86_64 arm64 armv7 x86_64-musl arm64-musl armv7-musl; do \
		rm -rf a4-$(VERSION)-$$arch && mkdir -p a4-$(VERSION)-$$arch; \
		cp -p a4-$$arch a4-$(VERSION)-$$arch/a4; \
		cp -p extras/a4-keycodes-$$arch a4-$(VERSION)-$$arch/a4-keycodes; \
		cp -p etc/*.ini a4-$(VERSION)-$$arch/; \
		sed "s/VERSION/$(VERSION)/g" < a4.1 > a4-$(VERSION)-$$arch/a4.1; \
		sed "s/VERSION/$(VERSION)/g" < extras/a4-keycodes.1 > a4-$(VERSION)-$$arch/a4-keycodes.1; \
		tar -czvf release/a4-$(VERSION)-$$arch.tar.gz a4-$(VERSION)-$$arch; \
		rm -rf a4-$(VERSION)-$$arch; \
	done
	sed "s/VERSION/$(VERSION)/g" < extras/install.sh > release/install.sh
	sed "s/VERSION/$(VERSION)/g" < extras/install-local.sh > release/install-local.sh
	cd release && sha256sum *.tar.gz install.sh install-local.sh > checksums.txt

clean:
	rm -f a4 extras/a4-keycodes $(a4_obj)

distclean: clean
	rm -f $(obj)
	rm -f a4-x86_64 a4-arm64 a4-armv7 a4-x86_64-musl a4-arm64-musl a4-armv7-musl \
		extras/a4-keycodes-x86_64 extras/a4-keycodes-arm64 extras/a4-keycodes-armv7 \
		extras/a4-keycodes-x86_64-musl extras/a4-keycodes-arm64-musl extras/a4-keycodes-armv7-musl
	rm -rf a4-$(VERSION)-*.tar.gz release

#### inih library, commit d6e9d1b 20221202 https://github.com/benhoyt/inih.git ####
lib/inih/ini.o: lib/inih/ini.h

#### unibilium library tag v2.0.0 commit e3b16d6 20180208 https://github.com/mauke/unibilium.git ####
TERMINFO_DIRS != ncursesw6-config --terminfo-dirs 2>/dev/null || \
	ncurses6-config  --terminfo-dirs 2>/dev/null || \
	ncursesw5-config --terminfo-dirs 2>/dev/null || \
	ncurses5-config  --terminfo-dirs 2>/dev/null || \
	echo "/etc/terminfo:/lib/terminfo:/usr/share/terminfo:/usr/lib/terminfo:/usr/local/share/terminfo:/usr/local/lib/terminfo"
unibilium_flags = -DTERMINFO_DIRS='"$(TERMINFO_DIRS)"' -Ilib/unibilium
lib/unibilium/unibilium.o: lib/unibilium/unibilium.h
lib/unibilium/uniutil.o: lib/unibilium/unibilium.h
lib/unibilium/uninames.o: lib/unibilium/unibilium.h

#### libtermkey v0.22 https://www.leonerd.org.uk/code/libtermkey/ ####
termkey_flags = -DHAVE_UNIBILIUM -Ilib/libtermkey
lib/libtermkey/termkey.o: lib/libtermkey/termkey.h lib/libtermkey/termkey-internal.h
lib/libtermkey/driver-csi.o: lib/libtermkey/termkey.h lib/libtermkey/termkey-internal.h
lib/libtermkey/driver-ti.o: lib/libtermkey/termkey.h lib/libtermkey/termkey-internal.h

#### libtickit revision 810 20221204 https://bazaar.leonerd.org.uk/c/libtickit/ ####
tickit_flags = -Ilib/libtickit/include
lib/libtickit/src/bindings.o: lib/libtickit/include/tickit.h lib/libtickit/src/bindings.h
lib/libtickit/src/debug.o: lib/libtickit/include/tickit.h
lib/libtickit/src/evloop-default.o: lib/libtickit/include/tickit.h lib/libtickit/include/tickit-evloop.h
lib/libtickit/src/pen.o: lib/libtickit/include/tickit.h lib/libtickit/src/bindings.h
lib/libtickit/src/rect.o: lib/libtickit/include/tickit.h
lib/libtickit/src/rectset.o: lib/libtickit/include/tickit.h
lib/libtickit/src/renderbuffer.o: lib/libtickit/include/tickit.h lib/libtickit/src/linechars.inc
lib/libtickit/src/string.o: lib/libtickit/include/tickit.h
lib/libtickit/src/term.o: lib/libtickit/include/tickit.h lib/libtickit/include/tickit-termdrv.h \
	lib/libtickit/src/bindings.h lib/libtickit/src/termdriver.h lib/libtickit/src/xterm-palette.inc
lib/libtickit/src/termdriver-ti.o: lib/libtickit/include/tickit.h lib/libtickit/include/tickit-termdrv.h \
	lib/libtickit/src/termdriver.h
lib/libtickit/src/termdriver-xterm.o: lib/libtickit/include/tickit.h lib/libtickit/include/tickit-termdrv.h \
	lib/libtickit/src/termdriver.h
lib/libtickit/src/tickit.o: lib/libtickit/include/tickit.h lib/libtickit/include/tickit-evloop.h
lib/libtickit/src/utf8.o: lib/libtickit/include/tickit.h lib/libtickit/src/unicode.h lib/libtickit/src/fullwidth.inc
lib/libtickit/src/window.o: lib/libtickit/include/tickit.h lib/libtickit/src/bindings.h

#### libvterm revision 826 20230126 https://bazaar.leonerd.org.uk/c/libvterm/ ####
vterm_flags = -Ilib/libvterm/include
lib/libvterm/src/encoding.o: lib/libvterm/include/vterm.h lib/libvterm/include/vterm_keycodes.h \
	lib/libvterm/src/vterm_internal.h lib/libvterm/src/encoding/DECdrawing.inc lib/libvterm/src/encoding/uk.inc
lib/libvterm/src/keyboard.o: lib/libvterm/include/vterm.h lib/libvterm/include/vterm_keycodes.h \
	lib/libvterm/src/utf8.h lib/libvterm/src/vterm_internal.h
lib/libvterm/src/mouse.o: lib/libvterm/include/vterm.h lib/libvterm/include/vterm_keycodes.h \
	lib/libvterm/src/utf8.h lib/libvterm/src/vterm_internal.h
lib/libvterm/src/parser.o: lib/libvterm/include/vterm.h lib/libvterm/include/vterm_keycodes.h \
	lib/libvterm/src/vterm_internal.h
lib/libvterm/src/pen.o: lib/libvterm/include/vterm.h lib/libvterm/include/vterm_keycodes.h \
	lib/libvterm/src/vterm_internal.h
lib/libvterm/src/screen.o: lib/libvterm/include/vterm.h lib/libvterm/include/vterm_keycodes.h \
	lib/libvterm/src/rect.h lib/libvterm/src/utf8.h lib/libvterm/src/vterm_internal.h
lib/libvterm/src/state.o: lib/libvterm/include/vterm.h lib/libvterm/include/vterm_keycodes.h \
	lib/libvterm/src/vterm_internal.h
lib/libvterm/src/unicode.o: lib/libvterm/include/vterm.h lib/libvterm/include/vterm_keycodes.h \
	lib/libvterm/src/vterm_internal.h lib/libvterm/src/fullwidth.inc
lib/libvterm/src/vterm.o: lib/libvterm/include/vterm.h lib/libvterm/include/vterm_keycodes.h \
	lib/libvterm/src/vterm_internal.h

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f a4 $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/a4
	cp -f extras/a4-keycodes $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/a4-keycodes
	mkdir -p $(DESTDIR)$(SYSCONFDIR)/a4
	cp -f etc/*.ini $(DESTDIR)$(SYSCONFDIR)/a4
	chmod 644 $(DESTDIR)$(SYSCONFDIR)/a4/*.ini
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < a4.1 > $(DESTDIR)$(MANPREFIX)/man1/a4.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/a4.1
	sed "s/VERSION/$(VERSION)/g" < extras/a4-keycodes.1 > $(DESTDIR)$(MANPREFIX)/man1/a4-keycodes.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/a4-keycodes.1
	mkdir -p $(DESTDIR)$(DOCDIR)/a4
	cp -f LICENSE $(DESTDIR)$(DOCDIR)/a4
	chmod 644 $(DESTDIR)$(DOCDIR)/a4/LICENSE

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/a4 \
		$(DESTDIR)$(PREFIX)/bin/a4-keycodes \
		$(DESTDIR)$(SYSCONFDIR)/a4/* \
		$(DESTDIR)$(MANPREFIX)/man1/a4*.1 \
		$(DESTDIR)$(DOCDIR)/a4/*
