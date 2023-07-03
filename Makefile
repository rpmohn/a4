PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man
DATAROOTDIR = ${PREFIX}/share
SYSCONFDIR = ${DATAROOTDIR}
DOCDIR = ${DATAROOTDIR}/doc/a4

LIBS = -lutil -ltickit -lvterm
LDFLGS = ${LDFLAGS} ${LIBS}

VERSION = $(shell git describe --always --dirty 2>/dev/null || echo "v0.2.2")
CPPFLGS = ${CPPFLAGS} -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_XOPEN_SOURCE_EXTENDED
CFLGS = ${CFLAGS} -std=c99 ${INCS} -DNDEBUG -DSYSCONFDIR=\"${DESTDIR}${SYSCONFDIR}\" ${CPPFLGS} -DVERSION=\"${VERSION}\"
DEBUG_CFLGS = ${CFLGS} -UNDEBUG -O0 -g -ggdb -Wall -Wextra -Wno-unused-parameter -fdiagnostics-color=always

CC ?= cc

SRC = a4.c lib/ini.c
DEP = layouts.c config.c vt.c utilities.c lib/ini.h lib/utf8.h lib/keynames.inc lib/rgb.inc Makefile

all: a4 extras/a4-keycodes

a4: ${SRC} ${DEP}
	${CC} ${CFLGS} ${SRC} ${LDFLGS} -o $@

extras/a4-keycodes: extras/a4-keycodes.c
	${CC} ${CFLAGS} $? ${LDFLAGS} -ltickit -o $@

debug: clean
	@$(MAKE) CFLGS='${DEBUG_CFLGS}'

clean:
	@echo cleaning
	@rm -f a4 extras/a4-keycodes

install: all
	@echo "installing application files to ${DESTDIR}${PREFIX}/bin"
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@for x in a4 extras/a4-keycodes; do \
		echo "	${DESTDIR}${PREFIX}/bin/$${x#extras/}"; \
		cp -f "$$x" "${DESTDIR}${PREFIX}/bin" && \
		chmod 755 "${DESTDIR}${PREFIX}/bin/$${x#extras/}"; \
	done
	@echo "installing INI files to ${DESTDIR}${SYSCONFDIR}/a4"
	@mkdir -p ${DESTDIR}${SYSCONFDIR}/a4
	@for x in etc/*.ini; do \
		echo "	${DESTDIR}${SYSCONFDIR}/a4/$${x#etc/}"; \
		cp -f "$$x" "${DESTDIR}${SYSCONFDIR}/a4" && \
		chmod 644 "${DESTDIR}${SYSCONFDIR}/a4/$${x#etc/}"; \
	done
	@echo "installing man files to ${DESTDIR}${MANPREFIX}"
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < a4.1 > ${DESTDIR}${MANPREFIX}/man1/a4.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/a4.1
	@echo "installing ${DESTDIR}${DOCDIR} files"
	@mkdir -p ${DESTDIR}${DOCDIR}
	@for x in LICENSE; do \
		echo "	${DESTDIR}${DOCDIR}/$$x"; \
		cp -f "$$x" "${DESTDIR}${DOCDIR}" && \
		chmod 644 "${DESTDIR}${DOCDIR}/$$x"; \
	done

uninstall:
	@echo "removing application files from ${DESTDIR}${PREFIX}/bin"
	@for x in a4 a4-keycodes; do \
		echo "	${DESTDIR}${PREFIX}/bin/$$x"; \
		rm -f "${DESTDIR}${PREFIX}/bin/$$x"; \
	done
	@echo "removing INI files from ${DESTDIR}${SYSCONFDIR}/a4"
	@for x in "${DESTDIR}${SYSCONFDIR}/a4"/*; do \
		echo "	"$$x""; \
		rm -f "$$x"; \
	done
	@echo "removing man files from ${DESTDIR}${MANPREFIX}"
	rm -f ${DESTDIR}${MANPREFIX}/man1/a4.1
	@echo "removing ${DESTDIR}${DOCDIR} files"
	@for x in "${DESTDIR}${DOCDIR}"/*; do \
		echo "	"$$x""; \
		rm -f "$$x"; \
	done

.PHONY: all debug clean install uninstall
