enum {
	LAYOUTS(XENUM)
};

/* local function declarations */
static void zoom_left_right(int type);
static void zoom_top_bot(int type);
static void grid_col_row(int type);

/* functions */
static void zoom_left_right(int type) {
	unsigned int i, num, znum;
	int flines, zcols, zlines, slines, divcol;
	bool noneminimized = true;
	wchar_t tee;
	TickitRect tfrect;
	TFrame *tframe;

	for (num = 0, tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
		if (!tframe->minimized)
			num++;
		else
			noneminimized = false;
	}

	flines = frame.rect.lines - (noneminimized ? 0 : 1);
	znum = MAX(1, MIN(num, currzoomnum));
	zcols = (num <= znum ? frame.rect.cols : currzoomsize * frame.rect.cols);
	zlines = flines / znum;
	slines = (num <= znum ? 0 : flines / (num - znum));

	tfrect.top = 0;
	tfrect.left = (type == LAYOUT_LEFT ? 0 : frame.rect.cols - zcols);

	tee = (type == LAYOUT_LEFT ? FRAME_LTEE : FRAME_RTEE);

	for (i = 0, tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
		if (tframe->minimized)
			continue;
		if (i < znum) {	/* zoom window */
			tfrect.cols = zcols;
			tfrect.lines = ((i < znum - 1) ? zlines : flines - tfrect.top);
		} else {	/* stack window */
			if (i == znum) {	/* (i == znum) for the first tframe in the stack */
				tfrect.top = 0;
				divcol = (tfrect.left += (type == LAYOUT_LEFT ? zcols : -1));
				add_frame_vline(tfrect.top, tfrect.left, flines);
				add_frame_char(tfrect.top, tfrect.left, FRAME_TTEE);
				tfrect.left = (type == LAYOUT_LEFT ? tfrect.left + 1 : 0);
				tfrect.cols = frame.rect.cols - zcols - 1;
			}
			tfrect.lines = ((i < num - 1) ? slines : flines - tfrect.top);
			if (i > znum)
				add_frame_char(tfrect.top, divcol, tee);
		}
		resize_term(tframe, tfrect);
		tfrect.top += tfrect.lines;
		i++;
	}

	/* Fill in zoomnum intersections */
	tee = (type == LAYOUT_LEFT ? FRAME_RTEE : FRAME_LTEE);
	if (num > znum) {
		tfrect.top = zlines;
		for (i = 1; i < znum; i++) {
			add_frame_char(tfrect.top, divcol, (tfrect.top % slines ? tee : FRAME_PLUS));
			tfrect.top += zlines;
		}
	}
}
static void zoom_left(void) { zoom_left_right(LAYOUT_LEFT); }
static void zoom_right(void) { zoom_left_right(LAYOUT_RIGHT); }

static void zoom_top_bot(int type) {
	unsigned int i, num, znum, mod;
	int flines, zcols, zlines, scols;
	bool noneminimized = true;
	TickitRect tfrect;
	TFrame *tframe;

	for (num = 0, tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
		if (!tframe->minimized)
			num++;
		else
			noneminimized = false;
	}

	flines = frame.rect.lines - (noneminimized ? 0 : 1);
	znum  = MAX(1, MIN(num, currzoomnum));
	zlines = (num <= znum ? flines : currzoomsize * (flines + 1));
	zcols = (frame.rect.cols / znum);
	scols = (num <= znum ? 0 : frame.rect.cols / (num - znum));

	tfrect.left = 0;
	tfrect.top = (type == LAYOUT_TOP ? 0 : flines - zlines);

	for (i = 0, tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
		if (tframe->minimized)
			continue;
		if (i < znum) {	/* zoom window */
			if (i > 0) {
				add_frame_vline(tfrect.top, tfrect.left - 1, tfrect.lines);
				add_frame_char(tfrect.top, tfrect.left - 1, FRAME_TTEE);
			}
			tfrect.lines = zlines;
			tfrect.cols = (i < znum - 1 ? zcols : frame.rect.cols - tfrect.left);
		} else {	/* stack window */
			if (i == znum) {	/* (i == znum) for the first tframe in the stack */
				tfrect.left = 0;
				tfrect.top = (type == LAYOUT_TOP ? tfrect.top + zlines : 0);
				tfrect.lines = flines - tfrect.top - (type == LAYOUT_TOP ? 0 : zlines);
			}
			if (i > znum) {
				add_frame_vline(tfrect.top, tfrect.left - 1, tfrect.lines);
				add_frame_char(tfrect.top, tfrect.left - 1, FRAME_TTEE);
			}
			tfrect.cols = (i < num - 1 ? scols : frame.rect.cols - tfrect.left);
		}
		resize_term(tframe, tfrect);
		tfrect.left += tfrect.cols + 1;
		i++;
	}

	if (type == LAYOUT_BOT)
		tfrect.top = flines - zlines;

	/* Fill in zoomnum intersections */
	if (num > znum) {
		tfrect.left = 0;
		for (i = 0; i < znum; i++) {
			if (i > 0) {
				mod = (tfrect.left - i) % (type == LAYOUT_TOP ? zcols : scols);
				add_frame_char(tfrect.top, tfrect.left - 1, (mod ? FRAME_TTEE : FRAME_PLUS));
			}
			tfrect.cols = (i < znum - 1 ? zcols : frame.rect.cols - tfrect.left);
			tfrect.left += tfrect.cols + 1;
		}
	}
}
static void zoom_bottom(void) { zoom_top_bot(LAYOUT_BOT); }
static void zoom_top(void) { zoom_top_bot(LAYOUT_TOP); }

static void grid_col_row(int type) {
	unsigned int flines, i, num, aw, ah, cols, rows;
	bool noneminimized = true;
	TickitRect tfrect, resizerect;
	TFrame *tframe;

	for (num = 0, tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
		if (!tframe->minimized)
			num++;
		else
			noneminimized = false;
	}

	/* grid dimensions */
	switch (type) {
	case LAYOUT_GRID:
		for (cols = 0; cols <= num / 2; cols++)
			if (cols * cols >= num)
				break;
		rows = (cols && (cols - 1) * cols >= num) ? cols - 1 : cols;
		break;
	case LAYOUT_COLS:
		cols = num;
		rows = 1;
		break;
	case LAYOUT_ROWS:
		cols = 1;
		rows = num;
		break;
	default:
		return;
	}

	/* window geoms (cell height/width) */
	flines = frame.rect.lines - (noneminimized ? 0 : 1);
	tfrect.lines = flines / (rows ? rows : 1);
	tfrect.cols = frame.rect.cols / (cols ? cols : 1);

	for (i = 0, tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
		if (tframe->minimized)
			continue;
		/* if there are less tframes in the last row than normal adjust the
		 * split rate to fill the empty space */
		if (rows > 1 && i == (rows * cols) - cols && (num - i) <= (num % cols))
			tfrect.cols = frame.rect.cols / (num - i);
		tfrect.left = (i % cols) * tfrect.cols;
		tfrect.top = (i / cols) * tfrect.lines;
		/* adjust height/width of last row/column's windows */
		ah = (i >= cols * (rows - 1)) ? flines - tfrect.lines * rows : 0;
		/* special case if there are less tframes in the last row */
		if (rows > 1 && i == num - 1 && (num - i) < (num % cols))
			/* (num % cols) == number of tframes in the last row */
			aw = frame.rect.cols - tfrect.cols * (num % cols);
		else
			aw = ((i + 1) % cols == 0) ? frame.rect.cols - tfrect.cols * cols : 0;
		if (i % cols) {
			add_frame_vline(tfrect.top, tfrect.left, tfrect.lines + ah);
			/* if we are on the first row, or on the last one and there are fewer tframes
			 * than normal whose border does not match the line above, print a top tree char
			 * otherwise a plus sign. */
			if (i <= cols
			    || (i >= rows * cols - cols && num % cols
				&& (cols - (num % cols)) % 2))
				add_frame_char(tfrect.top, tfrect.left, FRAME_TTEE);
			else
				add_frame_char(tfrect.top, tfrect.left, FRAME_PLUS);
			tfrect.left++, aw--;
		}
		resizerect = tfrect;
		resizerect.cols += aw;
		resizerect.lines += ah;
		resize_term(tframe, resizerect);
		i++;
	}
}
static void grid(void)    { grid_col_row(LAYOUT_GRID); }
static void columns(void) { grid_col_row(LAYOUT_COLS); }
static void rows(void)    { grid_col_row(LAYOUT_ROWS); }

static void fullscreen(void) {
	TickitRect rect = { 0, 0, frame.rect.lines, frame.rect.cols };

	for (TFrame *tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
		resize_term(tframe, rect);
	}
}

