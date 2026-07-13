/*

Copyright 2008, 2010, 2012 Lukas Mai.

This file is part of unibilium.

Unibilium is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Unibilium is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with unibilium.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "unibilium.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#ifdef _MSC_VER
# include <BaseTsd.h>
# define ssize_t SSIZE_T
#else
# include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef TERMINFO
#error "internal error: TERMINFO is not defined"
#endif
#ifndef TERMINFO_DIRS
#error "internal error: TERMINFO_DIRS is not defined"
#endif

enum {MAX_BUF = 32768};

const char *const unibi_terminfo_dirs = TERMINFO_DIRS;
const char *const unibi_terminfo = TERMINFO;

unibi_term *unibi_from_fp(FILE *fp) {
    char *buf = NULL;
    unibi_term *term = NULL;
    size_t n, r;

    if (!(buf = malloc(MAX_BUF))) {
        return term;
    }

    for (n = 0; n < MAX_BUF && (r = fread(buf + n, 1, MAX_BUF - n, fp)) > 0; ) {
        n += r;

        if (feof(fp)) {
            break;
        }
    }

    if (!ferror(fp)) {
        term = unibi_from_mem(buf, n);
    }

    free(buf);
    return term;
}

unibi_term *unibi_from_fd(int fd) {
    char *buf = NULL;
    unibi_term *term = NULL;
    size_t n;
    ssize_t r;

    if (!(buf = malloc(MAX_BUF))) {
        return term;
    }

    for (n = 0; n < MAX_BUF && (r = read(fd, buf + n, MAX_BUF - n)) > 0; ) {
        n += r;
    }

    if (r >= 0) {
        term = unibi_from_mem(buf, n);
    }

    free(buf);
    return term;
}

unibi_term *unibi_from_file(const char *file) {
    int fd;
    unibi_term *ut;

    if ((fd = open(file, O_RDONLY)) < 0) {
        return NULL;
    }

    ut = unibi_from_fd(fd);
    close(fd);
    return ut;
}

static int add_overflowed(size_t *dst, size_t src) {
    *dst += src;
    return *dst < src;
}

static unibi_term *from_dir(const char *dir_begin, const char *dir_end, const char *mid, const char *term) {
    char *path;
    unibi_term *ut;
    size_t dir_len, mid_len, term_len, path_size;

    dir_len = dir_end ? (size_t)(dir_end - dir_begin) : strlen(dir_begin);
    mid_len = mid ? strlen(mid) + 1 : 0;
    term_len = strlen(term);

    path_size = 0;
    if (
        add_overflowed(&path_size, dir_len) ||
        add_overflowed(&path_size, mid_len) ||
        add_overflowed(&path_size, term_len) ||
        add_overflowed(&path_size, 1 + 2           + 1 + 1)
                                /* /   (%c | %02x)   /   \0 */
    ) {
        /* overflow */
        errno = ENOMEM;
        return NULL;
    }
    if (!(path = malloc(path_size))) {
        return NULL;
    }

    memcpy(path, dir_begin, dir_len);
    sprintf(path + dir_len, "/" "%s"            "%s"             "%c" "/" "%s",
                                 mid ? mid : "", mid ? "/" : "",  term[0], term);

    errno = 0;
    ut = unibi_from_file(path);
    if (!ut && errno == ENOENT) {
        /* OS X likes to use /usr/share/terminfo/<hexcode>/name instead of the first letter */
        sprintf(path + dir_len + 1 + mid_len, "%02x/%s",
                                               (unsigned int)((unsigned char)term[0] & 0xff),
                                               term);
        ut = unibi_from_file(path);
    }
    free(path);
    return ut;
}

static unibi_term *from_dirs(const char *list, const char *term) {
    const char *a, *z;

    /* bool to check if unibi_terminfo already searched.
     * Init to 0, except if unibi_terminfo is empty */
    int unibi_terminfo_searched = unibi_terminfo[0] == '\0';

    if (list[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }

    a = list;

    for(unibi_term *ut = NULL;;) {
        z = strchr(a, ':');

        /* Empty entry can be either:
         *  - iterator is on the delimiter, i.e. delimiter starts the list or adjacent,
         *  - list ends with delimiter. */
        if (a != z && *a != '\0') {
            ut = from_dir(a, z, NULL, term);
        } else if (!unibi_terminfo_searched) {
            ut = from_dir(unibi_terminfo, NULL, NULL, term);
            unibi_terminfo_searched = 1;
        } else {
            errno = 0;
        }

        /* Return if success or error is other than not found (ut is NULL in that case) */
        if (ut || (errno != ENOENT && errno != EPERM && errno != EACCES)) {
            return ut;
        }

        /* If end of list */
        if (!z) {
            errno = ENOENT;
            return NULL;
        }

        a = z + 1;
    }
}

unibi_term *unibi_from_term(const char *term) {
    unibi_term *ut;
    const char *env;

    assert(term != NULL);

    if (term[0] == '\0' || term[0] == '.' || strchr(term, '/')) {
        errno = EINVAL;
        return NULL;
    }

    if ((env = getenv("TERMINFO"))) {
        ut = from_dir(env, NULL, NULL, term);
        if (ut) {
            return ut;
        }
    }

    if ((env = getenv("HOME"))) {
        ut = from_dir(env, NULL, ".terminfo", term);

        if (ut) {
            return ut;
        }

        if (errno != ENOENT && errno != EPERM && errno != EACCES) {
            return NULL;
        }
    }

    if ((env = getenv("TERMINFO_DIRS"))) {
        ut =  from_dirs(env, term);

        if (ut) {
            return ut;
        }

        if (errno != ENOENT && errno != EPERM && errno != EACCES) {
            return NULL;
        }
    }

    return from_dirs(unibi_terminfo_dirs, term);
}

unibi_term *unibi_from_env(void) {
    const char *term = getenv("TERM");
    if (!term) {
        errno = ENOENT;
        return NULL;
    }

    return unibi_from_term(term);
}
