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

static void tickit_pen_set_palette_colour(TickitPen *pen, TickitPenAttr attr, VTermColor *col, ColorScheme *cs);
static void TickitPen_from_VTermScreenCell(TickitPen *pen, VTermScreenCell *cell, ColorScheme *cs);
static void applycolorrules(TFrame *tframe);
static bool compare_cells(VTermScreenCell *a, VTermScreenCell *b);
static void fetch_cell(TFrame *tframe, VTermPos pos, VTermScreenCell *cell);
static VTermKey strp_key(const char *str);
static ssize_t pty_write(int fd, const char *buf, size_t len);
static int pty_read(Tickit *t, TickitEventFlags flags, void *_info, void *data);
static pid_t vt_forkpty(TFrame *tframe, const char *p, const char *argv[], const char *env[]);
static void free_vterm(TFrame *tframe);
static void get_vterm(TFrame *tframe);

/* functions */
static int vts_damage(VTermRect vrect, void *user) {
	TFrame *tframe = user;

	TickitRect rect = TickitRect_from_VTermRect(vrect);
	//DEBUG_LOGF("Uvd", "vts_damage vrect = %d/%d/%d/%d, rect = %d/%d/%d/%d", vrect.start_row, vrect.start_col, vrect.end_row, vrect.end_col, rect.top, rect.left, rect.lines, rect.cols);
	tickit_window_expose(tframe->termwin, &rect);
	return 1;
}

static int vts_moverect(VTermRect dst, VTermRect src, void *user) {
	TFrame *tframe = user;
	//DEBUG_LOGF("Uvr", "vts_moverect DST start_row %d, start_col %d, end_row %d, end_col %d",
	//		dst.start_row, dst.start_col, dst.end_row, dst.end_col);
	//DEBUG_LOGF("Uvr", "vts_moverect SRC  start_row %d, start_col %d, end_row %d, end_col %d",
	//		src.start_row, src.start_col, src.end_row, src.end_col);

	// FIXME: If returns false must redraw entire win? Could it be negative?
	if(src.start_col == dst.start_col) {
		tickit_window_scroll(tframe->termwin, src.start_row - dst.start_row, 0);
	} else {
		TickitRect r = {
			.top = src.start_row,
			.left = MIN(src.start_col, dst.start_col),
			.lines = (src.end_row - src.start_row),
			.cols = (src.end_col - src.start_col)
		};
		tickit_window_scrollrect(tframe->termwin, &r, src.start_row - dst.start_row, src.start_col - dst.start_col, NULL);
	}

	return 1;
}

static int vts_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user) {
	TFrame *tframe = user;
	//DEBUG_LOGF("Umc", "vts_movecursor from %d/%d to %d/%d", oldpos.row, oldpos.col, pos.row, pos.col);
	tickit_window_set_cursor_position(tframe->termwin, pos.row, pos.col);
	return 1;
}

static int vts_settermprop(VTermProp prop, VTermValue *val, void *user) {
	TFrame *tframe = user;

	if (prop == VTERM_PROP_TITLE) {
		if (val->string.initial)
			tframe->title[0] = '\0';

		int remaining = sizeof(tframe->title) - strlen(tframe->title) - 1;
		strncat(tframe->title, val->string.str, MIN(val->string.len, remaining));

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
	//DEBUG_LOGF("Uvp", "vts_sb_pushline start, cols = %d, sb_current = %d, sb_offset = %d", cols, tframe->sb_current, tframe->sb_offset);

	if (config.scroll_history == 0)
		return 0;

	/* copy vterm cells into sb_buffer */
	ScrollbackLine *sb_row = NULL;
	if (tframe->sb_current == config.scroll_history) {
		if (tframe->sb_buffer[tframe->sb_current - 1]->cols == cols)
			/* Recycle old row if it is the correct size */
			sb_row = tframe->sb_buffer[tframe->sb_current - 1];
		else
			free(tframe->sb_buffer[tframe->sb_current - 1]);

		/* Make room at the start by shifting to the right */
		memmove(tframe->sb_buffer + 1, tframe->sb_buffer, sizeof(tframe->sb_buffer[0]) * (tframe->sb_current - 1));
	} else if (tframe->sb_current > 0)
		/* Make room at the start by shifting to the right */
		memmove(tframe->sb_buffer + 1, tframe->sb_buffer, sizeof(tframe->sb_buffer[0]) * tframe->sb_current);

	if (!sb_row) {
		sb_row = malloc(sizeof(ScrollbackLine) + (cols * sizeof(sb_row->cells[0])));
		sb_row->cols = cols;
	}

	/* New row is added at the start of the storage buffer */
	tframe->sb_buffer[0] = sb_row;
	if (tframe->sb_current < config.scroll_history)
		tframe->sb_current++;

	memcpy(sb_row->cells, cells, cols * sizeof(cells[0]));

	DEBUG_LOGF("Uvp", "vts_sb_pushline end, cols = %d, sb_current = %d, sb_offset = %d", cols, tframe->sb_current, tframe->sb_offset);
	return 1;
}

static int vts_sb_popline(int cols, VTermScreenCell *cells, void *user) {
	TFrame *tframe = user;
	//DEBUG_LOGF("Uvp", "vts_sb_popline start, cols = %d, sb_current = %d, sb_offset = %d", cols, tframe->sb_current, tframe->sb_offset);

	if (!tframe->sb_current)
		return 0;

	ScrollbackLine *sb_row = tframe->sb_buffer[0];
	tframe->sb_current--;
	/* Forget the "popped" row by shifting the rest onto it */
	memmove(tframe->sb_buffer, tframe->sb_buffer + 1, sizeof(tframe->sb_buffer[0]) * (tframe->sb_current));

	int cols_to_copy = MIN(cols, sb_row->cols);

	/* copy to vterm state */
	memcpy(cells, sb_row->cells, sizeof(cells[0]) * cols_to_copy);
	//DEBUG_LOGF("Uvp", "vts_sb_popline cols = %d, sb_row->cols = %d, cols_to_copy = %d", cols, sb_row->cols, cols_to_copy);

	/* fill in end of line */
	for (int col = cols_to_copy; col < cols; col++) {
		//DEBUG_LOGF("Uvp", "vts_sb_popline col = %d, cols_to_copy = %d, cols = %d", col, cols_to_copy, cols);
		cells[col] = (VTermScreenCell){
			.chars = {0},
			.width = 1,
			.attrs = {},
			.fg = tframe->cs->fg,
			.bg = tframe->cs->bg,
		};
	}

	//DEBUG_LOGF("Uvp", "vts_sb_popline end, cols = %d, sb_current = %d, sb_offset = %d", cols, tframe->sb_current, tframe->sb_offset);
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
	tickit_pen_set_colour_attr_rgb8(pen, attr, (TickitPenRGB8){ .r = col->rgb.red, .g = col->rgb.green, col->rgb.blue });
	//printTickitPen(pen);
}

static void TickitPen_from_VTermScreenCell(TickitPen *pen, VTermScreenCell *cell, ColorScheme *cs) {
	tickit_pen_set_palette_colour(pen, TICKIT_PEN_FG, &cell->fg, cs);
	tickit_pen_set_palette_colour(pen, TICKIT_PEN_BG, &cell->bg, cs);

	tickit_pen_set_bool_attr(pen, TICKIT_PEN_BOLD, cell->attrs.bold);
	tickit_pen_set_int_attr(pen, TICKIT_PEN_UNDER, cell->attrs.underline);
	tickit_pen_set_bool_attr(pen, TICKIT_PEN_ITALIC, cell->attrs.italic);
	tickit_pen_set_bool_attr(pen, TICKIT_PEN_REVERSE, cell->attrs.reverse);
	tickit_pen_set_bool_attr(pen, TICKIT_PEN_STRIKE, cell->attrs.strike);
	tickit_pen_set_int_attr(pen, TICKIT_PEN_ALTFONT, cell->attrs.font);
	tickit_pen_set_bool_attr(pen, TICKIT_PEN_BLINK, cell->attrs.blink);
}

static void applycolorrules(TFrame *tframe) {
	const ColorRule *r;
	const ColorScheme *prev = tframe->cs;
	unsigned int i;

	for (i = 0; i < config.ncolorrules; i++) {
		r = &config.colorrules[i];
		if (strstr(tframe->title, r->title)) {
			tframe->cs = r->palette;
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

static bool compare_cells(VTermScreenCell *a, VTermScreenCell *b) {
	bool equal = true;
	equal = equal && vterm_color_is_equal(&a->fg, &b->fg);
	equal = equal && vterm_color_is_equal(&a->bg, &b->bg);
	equal = equal && (a->attrs.bold == b->attrs.bold);
	equal = equal && (a->attrs.underline == b->attrs.underline);
	equal = equal && (a->attrs.italic == b->attrs.italic);
	equal = equal && (a->attrs.reverse == b->attrs.reverse);
	equal = equal && (a->attrs.strike == b->attrs.strike);
	return equal;
}

static void fetch_cell(TFrame *tframe, VTermPos pos, VTermScreenCell *cell) {
	if (pos.row < 0) {
		ScrollbackLine *sb_row = tframe->sb_buffer[-pos.row - 1];
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
	//DEBUG_LOGF("Upr", "pty_read BEGIN");

	/* Linux kernel's PTY buffer is a fixed 4096 bytes (1 page) so there's */
	/* never any point reading more than that                            */
	char buffer[BUFSIZ];

	ssize_t bytes = read(fd, buffer, sizeof buffer);

	if (bytes < 0) {
		DEBUG_LOGF("Upr", "read pty fd %d failed - %s", fd, strerror(errno));
		tframe->died = true;
		return 0;
	}

	if (bytes == 0) {
		DEBUG_LOGF("Upr", "read pty fd %d returned 0 bytes - %s", fd, strerror(errno));
		quit(NULL);
		return 0;
	}

	//DEBUG_LOGF("Upr", "pty_read sending to vterm_input_write vt %p, bytes %d, buffer ::%.*s::", tframe->vt, bytes, bytes, buffer);
	//size_t written = vterm_input_write(tframe->vt, buffer, bytes);
	vterm_input_write(tframe->vt, buffer, bytes);
	//DEBUG_LOGF("Upr", "Sent %d bytes to vterm_input_write", written);
	vterm_screen_flush_damage(tframe->vts);

	//DEBUG_LOGF("Upr", "pty_read END");
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

static void get_vterm(TFrame *tframe) {
	const char *pargs[4] = { shell, NULL };

	tframe->vt = vterm_new(tframe->termrect.lines, tframe->termrect.cols);
	vterm_set_utf8(tframe->vt, true);
	tframe->vts = vterm_obtain_screen(tframe->vt);
	vterm_screen_reset(tframe->vts, 1);
	vterm_screen_set_damage_merge(tframe->vts, VTERM_DAMAGE_SCROLL);
	vterm_screen_enable_altscreen(tframe->vts, true);
	vterm_screen_enable_reflow(tframe->vts, true);
	vterm_screen_set_callbacks(tframe->vts, &vtermscreencallbacks, tframe);

	tframe->sb_current = tframe->sb_offset = 0;
	tframe->sb_buffer = malloc(sizeof(ScrollbackLine *) * config.scroll_history);

	tframe->worker_pid = vt_forkpty(tframe, shell, pargs, NULL);
	tframe->watchio = tickit_watch_io(root.tickit, tframe->controller_ptyfd, TICKIT_IO_IN|TICKIT_IO_HUP, 0, &pty_read, tframe);
}

static void free_vterm(TFrame *tframe) {
	if (tframe->sb_buffer)
		free(tframe->sb_buffer);
	if (tframe->watchio)
		tickit_watch_cancel(root.tickit, tframe->watchio);
	//DEBUG_LOGF("Ufv", "free_vterm vt = %p", tframe->vt);
	close(tframe->controller_ptyfd);
	if (tframe->vt)
		vterm_free(tframe->vt);
}
