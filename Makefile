.POSIX:
.PRAGMA: posix_202x
.SUFFIXES:
.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<
.PHONY: all debug clean distclean install uninstall

PREFIX		= /usr/local
MANPREFIX	= $(PREFIX)/share/man
SYSCONFDIR	= $(PREFIX)/share
DOCDIR		= $(PREFIX)/share/doc

VERSION		!= git describe --always --dirty 2>/dev/null || echo "v0.2.3"
CPPFLAGS	= -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_XOPEN_SOURCE_EXTENDED \
			  -DNDEBUG -DSYSCONFDIR='"$(SYSCONFDIR)"' $(CPPFLGS) -DVERSION='"$(VERSION)"' \
			  $(unibilium_flags) $(termkey_flags) $(tickit_flags) $(vterm_flags)
CFLAGS		= -std=c99 -Wall
DEBUG		= -UNDEBUG -O0 -g -ggdb -Wextra -Wno-unused-parameter -fdiagnostics-color=always
LDLIBS		= -lutil

a4_obj		= a4.o
inih_obj	= lib/inih/ini.o
uni_obj		= lib/unibilium/unibilium.o lib/unibilium/uninames.o lib/unibilium/uniutil.o
termkey_obj	= lib/libtermkey/termkey.o lib/libtermkey/driver-csi.o lib/libtermkey/driver-ti.o
tickit_obj	= lib/libtickit/src/bindings.o lib/libtickit/src/debug.o lib/libtickit/src/evloop-default.o \
			  lib/libtickit/src/pen.o lib/libtickit/src/rect.o lib/libtickit/src/rectset.o \
			  lib/libtickit/src/renderbuffer.o lib/libtickit/src/string.o lib/libtickit/src/term.o \
			  lib/libtickit/src/termdriver-ti.o lib/libtickit/src/termdriver-xterm.o \
			  lib/libtickit/src/tickit.o lib/libtickit/src/utf8.o lib/libtickit/src/window.o
vterm_obj	= lib/libvterm/src/encoding.o lib/libvterm/src/keyboard.o lib/libvterm/src/parser.o \
			  lib/libvterm/src/pen.o lib/libvterm/src/screen.o lib/libvterm/src/state.o \
			  lib/libvterm/src/unicode.o lib/libvterm/src/vterm.o
obj			= $(a4_obj) $(inih_obj) $(uni_obj) $(termkey_obj) $(tickit_obj) $(vterm_obj)

all: a4 extras/a4-keycodes

a4: $(obj)
	$(CC) $(LDFLAGS) -o $@ $(obj) $(LDLIBS)

a4.o: config.c layouts.c utilities.c vt.c lib/keynames.inc lib/rgb.inc lib/libvterm/src/utf8.h

extras/a4-keycodes: extras/a4-keycodes.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $? $(LDFLAGS) $(uni_obj) $(termkey_obj) $(tickit_obj) -o $@

debug: clean
	@$(MAKE) CFLAGS='$(CFLAGS) $(DEBUG)' a4

clean:
	rm -f a4 extras/a4-keycodes $(a4_obj)

distclean: clean
	rm -f $(obj)

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
	mkdir -p $(DESTDIR)$(DOCDIR)/a4
	cp -f LICENSE $(DESTDIR)$(DOCDIR)/a4
	chmod 644 $(DESTDIR)$(DOCDIR)/a4/LICENSE

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/a4 \
		$(DESTDIR)$(PREFIX)/bin/a4-keycodes \
		$(DESTDIR)$(SYSCONFDIR)/a4/* \
		$(DESTDIR)$(MANPREFIX)/man1/a4.1 \
		$(DESTDIR)$(DOCDIR)/a4/*
