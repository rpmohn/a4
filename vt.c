#include <pty.h>

#define TickitRect_from_VTermRect(v)      \
	{                                     \
		.top   = v.start_row,             \
		.left  = v.start_col,             \
		.lines = v.end_row - v.start_row, \
		.cols  = v.end_col - v.start_col, \
	}

/* function declarations */
static int vts_damage(VTermRect vrect, void *user);
static int vts_moverect(VTermRect dest, VTermRect src, void *user);
static int vts_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user);
static int vts_settermprop(VTermProp prop, VTermValue *val, void *user);
static int vts_bell(void *user);
static int vts_sb_pushline(int cols, const VTermScreenCell *cells, void *user);
static int vts_sb_popline(int cols, VTermScreenCell *cells, void *user);
static int vts_fallback_csi(const char *leader, const long args[], int argcount, const char *intermed, char command, void *user);
static int sync_timeout(Tickit *t, TickitEventFlags flags, void *info, void *user);
static int render_later(Tickit *t, TickitEventFlags flags, void *info, void *user);

static void tickit_pen_set_palette_colour(TickitPen *pen, TickitPenAttr attr, VTermColor *col, ColorScheme *cs);
static void applycolorrules(TFrame *tframe);
static void fetch_cell(TFrame *tframe, VTermPos pos, VTermScreenCell *cell);
static VTermKey strp_key(const char *str);
static ssize_t pty_write(int fd, const char *buf, size_t len);
static int pty_read(Tickit *t, TickitEventFlags flags, void *_info, void *data);
static pid_t vt_forkpty(TFrame *tframe, const char *p, const char *argv[], const char *env[]);
static void free_vterm(TFrame *tframe);
static void get_vterm_cmd(TFrame *tframe, const char *cmd, const char *argv[], const char *env[]);
static void get_vterm(TFrame *tframe);

/* functions */
static int vts_damage(VTermRect vrect, void *user) {
	TFrame *tframe = user;

	TickitRect rect = TickitRect_from_VTermRect(vrect);

	if (tframe->sync_update) {
		if (!tframe->sync_has_damage) {
			tframe->sync_rect = rect;
			tframe->sync_has_damage = true;
		} else {
			int top    = MIN(tframe->sync_rect.top,  rect.top);
			int left   = MIN(tframe->sync_rect.left, rect.left);
			int bottom = MAX(tframe->sync_rect.top  + tframe->sync_rect.lines,
			                 rect.top  + rect.lines);
			int right  = MAX(tframe->sync_rect.left + tframe->sync_rect.cols,
			                 rect.left + rect.cols);
			tickit_rect_init_bounded(&tframe->sync_rect, top, left, bottom, right);
		}
		return 1;
	}

	tickit_window_expose(tframe->termwin, &rect);
	return 1;
}

static int vts_moverect(VTermRect dst, VTermRect src, void *user) {
	/* Return 0 to indicate we can't handle the scroll, so libvterm will
	 * fall back to calling the damage callback with updated cell contents. */
	return 0;
}

static int vts_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user) {
	TFrame *tframe = user;
	/* defer to render_later; calling tickit_window_set_cursor_position here
	 * triggers a blocking write every tick, stalling the PTY drain loop */
	tframe->pending_cursor = pos;
	return 1;
}

static int vts_settermprop(VTermProp prop, VTermValue *val, void *user) {
	TFrame *tframe = user;

	if (prop == VTERM_PROP_TITLE) {
		if (val->string.initial)
			tframe->title[0] = '\0';

		size_t cur_len = strlen(tframe->title);
		size_t remaining = sizeof(tframe->title) - 1 - cur_len;
		size_t n = val->string.len < remaining ? val->string.len : remaining;
		memcpy(tframe->title + cur_len, val->string.str, n);
		tframe->title[cur_len + n] = '\0';

		if (val->string.final) {
			applycolorrules(tframe);
			tickit_window_expose(tframe->tbar, NULL);
			tickit_term_setctl_str(root.tt, TICKIT_TERMCTL_ICONTITLE_TEXT, tframe->title);
		}
	} else if (prop == VTERM_PROP_CURSORVISIBLE) {
		tickit_window_set_cursor_visible(tframe->termwin, val->boolean);
	} else if (prop == VTERM_PROP_CURSORBLINK) {
		tickit_window_setctl_int(tframe->termwin, TICKIT_WINCTL_CURSORBLINK, val->boolean);
	} else if (prop == VTERM_PROP_CURSORSHAPE) {
		tickit_window_set_cursor_shape(tframe->termwin, val->number);
	} else if (prop == VTERM_PROP_ALTSCREEN) {
		tframe->altscreen = val->boolean;
	}

	return 1;
}

static int vts_bell(void *user) {
	TFrame *tframe = user;
	tframe->urgent = true;
	redraw(NULL);
	return 1;
}
static int vts_sb_pushline(int cols, const VTermScreenCell *cells, void *user) {
	TFrame *tframe = user;

	if (config.scroll_history == 0)
		return 0;

	int capacity = config.scroll_history;
	int new_head = (tframe->sb_head - 1 + capacity) % capacity;
	ScrollbackLine *sb_row = NULL;

	if (tframe->sb_current == capacity) {
		/* Buffer full: evict the oldest line, which occupies the new_head slot */
		ScrollbackLine *old = tframe->sb_buffer[new_head];
		tframe->sb_buffer[new_head] = NULL; /* clear before free so failure path is safe */
		if (old && old->cols == cols)
			sb_row = old; /* recycle allocation */
		else
			free(old);
	}

	if (!sb_row) {
		if ((size_t)cols > (SIZE_MAX - sizeof(ScrollbackLine)) / sizeof(sb_row->cells[0]))
			return 0;
		sb_row = malloc(sizeof(ScrollbackLine) + cols * sizeof(sb_row->cells[0]));
		if (!sb_row)
			return 0;
		sb_row->cols = cols;
		if (tframe->sb_current < capacity)
			tframe->sb_current++;
	}

	tframe->sb_head = new_head;
	tframe->sb_buffer[tframe->sb_head] = sb_row;
	const VTermLineInfo *li = vterm_state_get_lineinfo(vterm_obtain_state(tframe->vt), 0);
	sb_row->continuation = li ? li->continuation : 0;
	memcpy(sb_row->cells, cells, cols * sizeof(cells[0]));

	return 1;
}

static int vts_sb_popline(int cols, VTermScreenCell *cells, void *user) {
	TFrame *tframe = user;

	if (!tframe->sb_current)
		return 0;

	/* Pop the most recent line and advance head toward older entries */
	ScrollbackLine *sb_row = tframe->sb_buffer[tframe->sb_head];
	tframe->sb_buffer[tframe->sb_head] = NULL;
	tframe->sb_current--;
	tframe->sb_head = (tframe->sb_head + 1) % config.scroll_history;
	if (tframe->sb_offset > tframe->sb_current)
		tframe->sb_offset = tframe->sb_current;

	int cols_to_copy = MIN(cols, sb_row->cols);
	memcpy(cells, sb_row->cells, sizeof(cells[0]) * cols_to_copy);

	/* fill in end of line */
	for (int col = cols_to_copy; col < cols; col++) {
		cells[col] = (VTermScreenCell){
			.chars = {0},
			.width = 1,
			.attrs = {},
			.fg = tframe->cs->fg,
			.bg = tframe->cs->bg,
		};
	}

	free(sb_row);
	return 1;
}

VTermScreenCallbacks vtermscreencallbacks = {
	.damage      = vts_damage,
	.moverect    = vts_moverect,
	.movecursor  = vts_movecursor,
	.settermprop = vts_settermprop,
	.bell        = vts_bell,
	.sb_pushline = vts_sb_pushline,
	.sb_popline  = vts_sb_popline,
};

static void sync_flush(TFrame *tframe) {
	tframe->sync_update = false;
	tframe->sync_timer = NULL;
	if (tframe->sync_has_damage) {
		tickit_window_expose(tframe->termwin, &tframe->sync_rect);
		tframe->sync_has_damage = false;
	}
}

static int sync_timeout(Tickit *t, TickitEventFlags flags, void *info, void *user) {
	TFrame *tframe = user;
	if (tframe->render_timer) {
		tickit_watch_cancel(root.tickit, tframe->render_timer);
		tframe->render_timer = NULL;
	}
	tickit_window_set_cursor_position(tframe->termwin,
		tframe->pending_cursor.row, tframe->pending_cursor.col);
	vterm_screen_flush_damage(tframe->vts);
	sync_flush(tframe);
	return 0;
}

static int render_later(Tickit *t, TickitEventFlags flags, void *info, void *user) {
	TFrame *tframe = user;
	tframe->render_timer = NULL;
	tickit_window_set_cursor_position(tframe->termwin,
		tframe->pending_cursor.row, tframe->pending_cursor.col);
	vterm_screen_flush_damage(tframe->vts);
	return 0;
}

/* DEC 2026 synchronized output: ?2026h = begin, ?2026l = end */
static int vts_fallback_csi(const char *leader, const long args[], int argcount,
                            const char *intermed, char command, void *user) {
	TFrame *tframe = user;

	if (leader && leader[0] == '?' && leader[1] == '\0' &&
	    argcount == 1 && args[0] == 2026) {
		if (command == 'h') {
			tframe->sync_update = true;
			if (!tframe->sync_timer)
				tframe->sync_timer = tickit_watch_timer_after_msec(
					root.tickit, 1000, 0, &sync_timeout, tframe);
		} else if (command == 'l') {
			if (tframe->render_timer) {
				tickit_watch_cancel(root.tickit, tframe->render_timer);
				tframe->render_timer = NULL;
			}
			tickit_window_set_cursor_position(tframe->termwin,
				tframe->pending_cursor.row, tframe->pending_cursor.col);
			vterm_screen_flush_damage(tframe->vts);
			if (tframe->sync_timer) {
				tickit_watch_cancel(root.tickit, tframe->sync_timer);
				tframe->sync_timer = NULL;
			}
			sync_flush(tframe);
		}
		return 1;
	}

	return 0;
}

static VTermStateFallbacks vtermfallbacks = {
	.csi = vts_fallback_csi,
};

static void tickit_pen_set_palette_colour(TickitPen *pen, TickitPenAttr attr, VTermColor *col, ColorScheme *cs) {
	//printVTermColor(col);
	if (VTERM_COLOR_IS_INDEXED(col)) {
		if (cs != NULL)
			col = &cs->palette[col->indexed.idx];
		if (VTERM_COLOR_IS_INDEXED(col)) {
			tickit_pen_set_colour_attr(pen, attr, col->indexed.idx);
			return;
		}
	}

	tickit_pen_set_colour_attr(pen, attr, rgb_to_idx(col->rgb.red, col->rgb.green, col->rgb.blue));
	tickit_pen_set_colour_attr_rgb8(pen, attr, (TickitPenRGB8){ .r = col->rgb.red, .g = col->rgb.green, .b = col->rgb.blue });
	//printTickitPen(pen);
}

static void applycolorrules(TFrame *tframe) {
	const ColorRule *r;
	const ColorScheme *prev = tframe->cs;
	unsigned int i;

	for (i = 0; i < config.ncolorrules; i++) {
		r = &config.colorrules[i];
		if (strstr(tframe->title, r->title)) {
			tframe->cs = r->cs;
			break;
		}
	}

	/* if no match then use default */
	if (i >= config.ncolorrules)
		tframe->cs = config.colorschemes;

	/* if no change then leave it alone */
	if (tframe->cs == prev)
		return;

	vterm_screen_set_default_colors(tframe->vts, &tframe->cs->fg, &tframe->cs->bg);
	tickit_window_set_pen(tframe->termwin, tframe->cs->pen);
	tickit_window_expose(tframe->win, NULL);
}

static void fetch_cell(TFrame *tframe, VTermPos pos, VTermScreenCell *cell) {
	if (pos.row < 0) {
		if (config.scroll_history == 0) {
			memset(cell, 0, sizeof(*cell));
			return;
		}
		int idx = (tframe->sb_head + (-pos.row - 1)) % config.scroll_history;
		ScrollbackLine *sb_row = tframe->sb_buffer[idx];
		if (!sb_row) {
			memset(cell, 0, sizeof(*cell));
			return;
		}
		if (pos.col < sb_row->cols) {
			*cell = sb_row->cells[pos.col];
		} else {
			*cell = (VTermScreenCell){ { 0 }, .width = 1 };
			cell->bg = sb_row->cells[sb_row->cols - 1].bg;
		}
	} else {
		vterm_screen_get_cell(tframe->vts, pos, cell);
		//DEBUG_LOGF("Ufc", "fetch_cell %c for row %d, col %d", cell->chars[0], pos.row, pos.col);
	}
}

static VTermKey strp_key(const char *str) {
	/* from enum VTermKey in libvterm/include/vterm_keycodes.h */
	/* and struct keynames[] in libtermkey/termkey.c           */
	static struct {
		char *name;
		VTermKey key;
	} keynames[] = {
#include "lib/keynames.inc"
	};

	for(int i = 0; keynames[i].name; i++) {
		if(!strcmp(str, keynames[i].name))
			return keynames[i].key;
	}

	return VTERM_KEY_NONE;
}

static ssize_t pty_write(int fd, const char *buf, size_t len) {
	ssize_t ret = len;
	//DEBUG_LOGF("Upr", "pty_write");

	while (len > 0) {
		//DEBUG_LOGF("Upr", "pty_write sending to write fd %p, bytes %d, buf ::%*s::", fd, len, len, buf);
		ssize_t res = write(fd, buf, len);
		if (res < 0) {
			if (errno != EAGAIN && errno != EINTR)
				return -1;
			continue;
		}
		buf += res;
		len -= res;
	}

	return ret;
}

static int pty_read(Tickit *t, TickitEventFlags flags, void *_info, void *data) {
	TickitIOWatchInfo *info = _info;
	int fd = info->fd;
	TFrame *tframe = data;

	char buffer[BUFSIZ];
	bool got_data = false;

	/* Drain all available PTY data before flushing damage, so a burst of
	 * output (e.g. a full-screen app redraw) results in a single render
	 * pass rather than one per read. */
	for (;;) {
		ssize_t bytes = read(fd, buffer, sizeof buffer);
		if (bytes < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			DEBUG_LOGF("Upr", "read pty fd %d failed - %s", fd, strerror(errno));
			tframe->died = true;
			return 0;
		}
		if (bytes == 0) {
			DEBUG_LOGF("Upr", "read pty fd %d returned 0 bytes", fd);
			quit(NULL);
			return 0;
		}
		vterm_input_write(tframe->vt, buffer, bytes);
		got_data = true;
	}

	if (got_data && !tframe->render_timer)
		tframe->render_timer = tickit_watch_timer_after_msec(
			root.tickit, 16, 0, &render_later, tframe);

	return 1;
}

static pid_t vt_forkpty(TFrame *tframe, const char *p, const char *argv[], const char *env[]) {
	struct winsize ws = { tframe->termrect.lines, tframe->termrect.cols, 0, 0 };

	pid_t pid = forkpty(&tframe->controller_ptyfd, NULL, NULL, &ws);
	if (pid < 0)
		return -1;

	if (pid == 0) {
		setsid();

		sigset_t emptyset;
		sigemptyset(&emptyset);
		sigprocmask(SIG_SETMASK, &emptyset, NULL);

		int maxfd = sysconf(_SC_OPEN_MAX);
		for (int fd = 3; fd < maxfd; fd++)
			if (close(fd) == -1 && errno == EBADF)
				break;

		if (pending_cwd)
			chdir(pending_cwd);

		for (const char **envp = env; envp && envp[0]; envp += 2)
			setenv(envp[0], envp[1], 1);

		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_flags = 0;
		sigemptyset(&sa.sa_mask);
		sa.sa_handler = SIG_DFL;
		sigaction(SIGPIPE, &sa, NULL);

		execvp(p, (char *const *)argv);
		fprintf(stderr, "\nexecv() failed.\nCommand: '%s'\n", argv[0]);
		exit(1);
	}

	return pid;
}

static void get_vterm_cmd(TFrame *tframe, const char *cmd, const char *argv[], const char *env[]) {
	tframe->vt = vterm_new(tframe->termrect.lines, tframe->termrect.cols);
	vterm_set_utf8(tframe->vt, true);
	tframe->vts = vterm_obtain_screen(tframe->vt);
	vterm_screen_reset(tframe->vts, 1);
	vterm_screen_set_damage_merge(tframe->vts, VTERM_DAMAGE_SCROLL);
	vterm_screen_enable_altscreen(tframe->vts, true);
	vterm_screen_enable_reflow(tframe->vts, true);
	vterm_screen_set_callbacks(tframe->vts, &vtermscreencallbacks, tframe);
	vterm_screen_set_unrecognised_fallbacks(tframe->vts, &vtermfallbacks, tframe);

	tframe->sb_current = tframe->sb_offset = tframe->sb_head = 0;
	tframe->sb_buffer = calloc(config.scroll_history, sizeof(ScrollbackLine *));

	tframe->worker_pid = vt_forkpty(tframe, cmd, argv, env);
	int flags = fcntl(tframe->controller_ptyfd, F_GETFL, 0);
	fcntl(tframe->controller_ptyfd, F_SETFL, flags | O_NONBLOCK);
	tframe->watchio = tickit_watch_io(root.tickit, tframe->controller_ptyfd, TICKIT_IO_IN|TICKIT_IO_HUP, 0, &pty_read, tframe);
}

static void get_vterm(TFrame *tframe) {
	const char *pargs[] = { shell, NULL };
	get_vterm_cmd(tframe, shell, pargs, NULL);
}

static void free_vterm(TFrame *tframe) {
	if (tframe->sb_buffer) {
		for (int i = 0; i < tframe->sb_current; i++)
			free(tframe->sb_buffer[(tframe->sb_head + i) % config.scroll_history]);
		free(tframe->sb_buffer);
	}
	if (tframe->render_timer)
		tickit_watch_cancel(root.tickit, tframe->render_timer);
	if (tframe->sync_timer)
		tickit_watch_cancel(root.tickit, tframe->sync_timer);
	if (tframe->watchio)
		tickit_watch_cancel(root.tickit, tframe->watchio);
	//DEBUG_LOGF("Ufv", "free_vterm vt = %p", tframe->vt);
	close(tframe->controller_ptyfd);
	if (tframe->vt)
		vterm_free(tframe->vt);
}
