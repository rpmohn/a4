/* Window layout:
 * root contains sbar and frame (white)
 *     sbar displays a 1 line statusbar and can be toggled top/bottom on/off (magenta)
 *     frame contains tframes and displays separation lines between tframes (magenta)
 *         tframes is a linked list of tframes (green)
 *         each tframe contains a tbar and a termwin
 *             tbar displays a 1 line titlebasr at the top of each terminal (cyan)
 *             termwin contains the actual terminal (cyan)
 */
#include <errno.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include <tickit.h>
#include <vterm.h>

/* X macro expansion functions */
#define XACTION(n,...) static void n(char *args[]);
#define XLAYOUT(n,...) static void n(void);
#define XCHOOSE(n,...) { #n, n },
#define XENUM(n,e,...) e,

/* macros*/
#define TAGMASK     (unsigned int)((1 << config.ntags) - 1)
#define GROUPEDFOCUS (sel && sel->groupedfocus)
#define ARGS0EQ(a) (!strcmp(args[0], (a)))
#define MAX_STR BUFSIZ
#define MAX_COLORINDEX 256
#define DEBUG_LOGF  if(tickit_debug_enabled) tickit_debug_logf

#define FRAME_LTEE  L'├'
#define FRAME_RTEE  L'┤'
#define FRAME_TTEE  L'┬'
#define FRAME_VLINE L'│'
#define FRAME_PLUS  L'┼'

#include "utilities.c"

/* typedefs */
typedef struct {
	char *name;
	char *symbol;
	void (*arrange)(void);
} Layout;

typedef struct {
	char *name;
	VTermColor fg, bg, cursor;
	VTermColor palette[MAX_COLORINDEX];
	VTermScreenCell cell;
	TickitPen *pen;
} ColorScheme;

typedef struct {
	char *title;
	ColorScheme *cs;
} ColorRule;

/* KeyName is key plus optional modifier prefix of M-, C-, S-, M-C-, M-S-, C-S-, M-C-S-
 * longest possible keyname is M-C-S-Backspace, 6 + 9 chars + NULL = 17, round up to 24 */
#define MAX_KEYNAME 24
typedef char KeyName[MAX_KEYNAME];

#define MAX_KEYS 3
typedef KeyName KeyCombo[MAX_KEYS];

#define MAX_ARGS 3
typedef struct {
	void (*cmd)(char *args[]);
	char *args[MAX_ARGS];
} Action;

typedef struct {
	KeyCombo keys;
	Action action;
} KeyBinding;

/* globals */
static struct {
	Tickit *tickit;
	TickitWindow *win;
	TickitTerm *tt;
	TickitRect rect;
} root;

static struct {
	TickitWindow *win;
	TickitRect rect;
	int bind_expose;
	int bind_geomchange;
	void *watch_cmd;
	char text[MAX_STR];
	bool showbar;
	bool topbar;
} sbar;

typedef struct FrameLine FrameLine;
struct FrameLine {
	TickitRect line;
	wchar_t c;
	FrameLine *next;
};

typedef struct {
	int cols;
	VTermScreenCell cells[];
} ScrollbackLine;

static struct {
	TickitWindow *win;
	TickitRect rect;
	int bind_expose;
	int bind_geomchange;
	FrameLine *lines;
} frame;

typedef struct TFrame TFrame;
struct TFrame {
	TickitWindow *win;
	TickitRect rect;
	int bind_geomchange;
	char title[MAX_STR];
	ColorScheme *cs;
	unsigned int tags;
	unsigned int order;
	bool minimized;
	bool readonly;
	bool urgent;
	bool groupedfocus;
	volatile sig_atomic_t died;
	TFrame *next;
	TFrame *prev;
	TFrame *snext;

	TickitWindow *tbar;
	int tbar_bind_expose;
	int tbar_bind_geomchange;

	TickitWindow *termwin;
	TickitRect termrect;
	int termwin_bind_expose;
	int termwin_bind_geomchange;
	VTerm *vt;
	VTermScreen *vts;
	int controller_ptyfd;
	pid_t worker_pid;
	void *watchio;

	int sb_current;
	int sb_offset;
	ScrollbackLine **sb_buffer;
};
static TFrame *tframes = NULL; /* Doubly linked list of all tframes */
static TFrame *stack = NULL;   /* Stack ordered linked list of all tframes */
static TFrame *sel = NULL;     /* currently focused tframe */
static TFrame *lastsel = NULL; /* previously focused tframe */

typedef enum { NONE = 0, TERM, TBAR, TAG, LAYOUT, SBAR, FRAME } MWinType;
typedef struct {
	MWinType type;
	union {
		TFrame *tframe;
		int tag;
	};
} MWin;
static MWin mwin;

static struct {
	unsigned int curtag, prevtag;
	Layout **currlayout;
	char **lastlayout;
	int *currzoomnum;
	float *currzoomsize;
	bool *showbar;
	bool *topbar;
} pertag;

static const char *application_name = "a4";
static char *a4configfname = NULL;
static bool startups = true;
static unsigned int seltags;
static unsigned int tagset[2] = { 1, 1 };
static Layout *currlayout = NULL; /* current layout */
static int layoutsymlen = 0;      /* current layout symbol len */
static unsigned int currzoomnum;
static float currzoomsize;
static unsigned int sbar_cmd_num = 0; /* current statusbar command number */

static const char *shell;
static volatile sig_atomic_t running = true;

static KeyCombo curkeys;
static unsigned int curkeys_index = 0;

/* function declarations */
static void keypress(TickitKeyEventInfo *key, const char *seq);
static void curkeyscpy(const char *str);
static void curkeymouse(TickitMouseEventInfo *m);
static MWin *get_mwin_by_coord(int line, int col);
static TFrame *get_tframe_by_coord(int line, int col);
static KeyBinding *keybinding(KeyCombo keys, unsigned int keycount);
static KeyBinding *mousebinding(KeyCombo keys, unsigned int keycount);

static int resize_rootwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static int key_rootwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static int mouse_rootwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static int sigint_rootwin(Tickit *t, TickitEventFlags flags, void *info, void *data);
static void create_rootwin(void);
static void destroy_rootwin(void);

static int render_sbarwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static int resize_sbarwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static int get_status(Tickit *t, TickitEventFlags flags, void *_info, void *data);
static void create_sbarwin(void);
static void destroy_sbarwin(void);

static void draw_frameline(TickitRenderBuffer *rb, FrameLine *l);
static void add_frameline(TickitRect *line, wchar_t c);
static void add_frame_vline(int top, int left, int lines);
static void add_frame_char(int top, int left, wchar_t c);
static void clear_framelines(void);
static int render_framewin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static int resize_framewin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static void create_framewin(void);
static void destroy_framewin(void);

static int resize_tframewin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static void create_tframe(void);
static void destroy_tframe(TFrame *tframe);
static void expose_all_tbars(void);
static int render_tbarwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static int resize_tbarwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static void create_tbarwin(TFrame *tframe);
static void destroy_tbarwin(TFrame *tframe);
static int render_termwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static int resize_termwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data);
static void create_termwin(TFrame *tframe);
static void destroy_termwin(TFrame *tframe);

static void erase(void);
static bool isvisible(TFrame *tframe);
static bool is_content_visible(TFrame *tframe);
static TFrame *nextvisible(TFrame *tframe);
static void set_sbar_rect(void);
static void set_frame_rect(void);
static void arrange(void);
static void resize_term(TFrame *tframe, TickitRect rect);
static void attach(TFrame *tframe);
static void attachafter(TFrame *tframe, TFrame *a);
static void attachstack(TFrame *tframe);
static void detach(TFrame *tframe);
static void detachstack(TFrame *tframe);
static void dofocus(TFrame *tframe);
static void focusnextvis(void);
static void vscroll_delta(TFrame *tframe, int delta);
static unsigned int bitoftag(const char *tag);
static void tagschanged(void);
static void viewswap(void);
static void viewset(char *tagname);

static bool isarrange(void (*func)());
static bool hasstackarea(void);
static void set_pertag(void);
static void destroy_pertag(void);
static void create_pertag(void);
static void startup_a4(void);
static void shutdown_a4(void);
static void usage(const char *errstr, ...);
static void parse_args(int argc, char *argv[]);

#define ACTIONS(X)        \
	X(create)             \
	X(destroy)            \
	X(focus)              \
	X(keysequence)        \
	X(layout)             \
	X(minimize)           \
	X(noaction)           \
	X(quit)               \
	X(readonly)           \
	X(redraw)             \
	X(scrollback)         \
	X(statusbar)          \
	X(tag)                \
	X(tagtoggle)          \
	X(view)               \
	X(viewtoggle)         \
	X(zoom)               \
	X(zoomnum)            \
	X(zoomsize)
ACTIONS(XACTION)

#define LAYOUTS(X)              \
	X(zoom_left, LAYOUT_LEFT)   \
	X(zoom_right, LAYOUT_RIGHT) \
	X(zoom_top, LAYOUT_TOP)     \
	X(zoom_bottom, LAYOUT_BOT)  \
	X(grid, LAYOUT_GRID)        \
	X(columns, LAYOUT_COLS)     \
	X(rows, LAYOUT_ROWS)        \
	X(fullscreen, LAYOUT_FS)
LAYOUTS(XLAYOUT)

#include "layouts.c"
#include "config.c"
#include "vt.c"
#include "lib/utf8.h"

/* functions */
static void curkeyscpy(const char *str) {
	strncpy(curkeys[curkeys_index], str, MAX_KEYNAME);

	int len = strlen(str);
	switch(str[len - 1]) {
		case ' ':
			strncpy(curkeys[curkeys_index] + len - 1, "Space", MAX_KEYNAME - len);
			break;
		case '-':
			strncpy(curkeys[curkeys_index] + len - 1, "Hyphen", MAX_KEYNAME - len);
			break;
	}

	curkeys_index++;
}

static void curkeymouse(TickitMouseEventInfo *m) {
	char str[MAX_KEYNAME];
	char *s = str;

	s += sprintf(s, "%s%s%s",
			(m->mod & TICKIT_MOD_ALT ? "M-" : ""),
			(m->mod & TICKIT_MOD_CTRL ? "C-" : ""),
			(m->mod & TICKIT_MOD_SHIFT ? "S-" : ""));

	switch(m->type) {
		case TICKIT_MOUSEEV_WHEEL:
			sprintf(s, "wheel-%s", (m->button == TICKIT_MOUSEWHEEL_UP ? "up" : "dn"));
			break;
		case TICKIT_MOUSEEV_PRESS:
			sprintf(s, "press-%d", m->button);
			break;
		case TICKIT_MOUSEEV_DRAG:
			sprintf(s, "drag-%d", m->button);
			break;
		case TICKIT_MOUSEEV_RELEASE:
			sprintf(s, "release-%d", m->button);
			break;
		default:
			return;
	}

	strncpy(curkeys[curkeys_index], str, MAX_KEYNAME);
	curkeys_index++;
}

static MWin *get_mwin_by_coord(int line, int col) {
	MWin *mw;

	mw = calloc(1, sizeof(MWin));

	if (sel && isarrange(fullscreen)) {
		int top = frame.rect.top + sel->rect.top;
		int bottom = top + sel->rect.lines;
		int left = sel->rect.left;
		int right = left + sel->rect.cols;
		if ((col >= left) && (col < right) && (line >= top) && (line < bottom)) {
			mw->type = (line == top ? TBAR : TERM);
			mw->tframe = sel;
		}
	} else {
		for (TFrame *tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
			int top = frame.rect.top + tframe->rect.top;
			int bottom = top + tframe->rect.lines;
			int left = tframe->rect.left;
			int right = left + tframe->rect.cols;
			if ((col >= left) && (col < right) && (line >= top) && (line < bottom)) {
				mw->type = (line == top ? TBAR : TERM);
				mw->tframe = tframe;
				break;
			}
		}
	}

	if (mw->type == NONE) {
		if ((line >= sbar.rect.top) && (line < sbar.rect.top + sbar.rect.lines)) {
			//search for sbar, tag, layout
			mw->type = SBAR;
			int sbarcol = 0;
			for (unsigned int i = 0; i < config.ntags; i++) {
				sbarcol += config.taglens[i];
				if (col < sbarcol) {
					mw->type = TAG;
					mw->tag = i;
					break;
				}
			}
			if (mw->type == SBAR && col < (sbarcol + layoutsymlen)) {
				mw->type = LAYOUT;
			}
		} else if (sel)
			mw->type = FRAME;
	}

	return mw;
}

static TFrame *get_tframe_by_coord(int line, int col) {
	if (line < frame.rect.top || line >= frame.rect.top + frame.rect.lines)
		return NULL;
	if (isarrange(fullscreen))
		return sel;
	for (TFrame *tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
		int top = frame.rect.top + tframe->rect.top;
		int bottom = top + tframe->rect.lines;
		int left = tframe->rect.left;
		int right = left + tframe->rect.cols;
		if ((col >= left) && (col < right) && (line >= top) && (line < bottom))
			return tframe;
	}
	return NULL;
}

static KeyBinding *keybinding(KeyCombo keys, unsigned int keycount) {
	for (int b = config.nkb_bindings - 1; b >= 0; b--) {
		for (unsigned int k = 0; k < keycount; k++) {
			if (strcmp(keys[k], config.kb_bindings[b].keys[k]) != 0)
				break;
			if (k == keycount - 1)
				return &config.kb_bindings[b];
		}
	}
	return NULL;
}

static KeyBinding *mousebinding(KeyCombo keys, unsigned int keycount) {
	unsigned int n;
	KeyBinding *bindings;

	if (mwin.type == TERM) {
		n = config.nmterm_bindings;
		bindings = config.mterm_bindings;
	} else if (mwin.type == TBAR) {
		n = config.nmtbar_bindings;
		bindings = config.mtbar_bindings;
	} else if (mwin.type == TAG) {
		n = config.nmtag_bindings;
		bindings = config.mtag_bindings;
	} else if (mwin.type == LAYOUT) {
		n = config.nmlayout_bindings;
		bindings = config.mlayout_bindings;
	} else if (mwin.type == SBAR) {
		n = config.nmsbar_bindings;
		bindings = config.msbar_bindings;
	} else if (mwin.type == FRAME) {
		n = config.nmframe_bindings;
		bindings = config.mframe_bindings;
	} else
		return NULL;

	for (int b = n - 1; b >= 0; b--) {
		for (unsigned int k = 0; k < keycount; k++) {
			if (strcmp(keys[k], bindings[b].keys[k]) != 0)
				break;
			if (k == keycount - 1)
				return &bindings[b];
		}
	}
	return NULL;
}

static int resize_rootwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	TickitGeomchangeEventInfo *info = _info;
	//DEBUG_LOGF("Urt", "resize_rootwin top = %d, left = %d, lines = %d, cols = %d", info->rect.top, info->rect.left, info->rect.lines, info->rect.cols);
	root.rect = info->rect;
	arrange();
	return 1;
}

/* BEGIN code from https://dev.to/rdentato/utf-8-strings-in-c-2-3-3kp1 */
static uint8_t const u8_length[] = {
// 0 1 2 3 4 5 6 7 8 9 A B C D E F
   1,1,1,1,1,1,1,1,0,0,0,0,2,2,3,4
};

#define u8length(s) u8_length[(((uint8_t *)(s))[0] & 0xFF) >> 4];

static uint32_t str_to_utf8encoding(const char *str) {
	uint32_t encoding = 0;
	int len = u8length(str);
	for (int i = 0; i < len && str[i] != '\0'; i++)
		encoding = (encoding << 8) | (str[i] & 0xFF);
	return encoding;
}

static uint32_t str_to_codepoint(const char *str) {
	uint32_t c = str_to_utf8encoding(str);
	if (c > 0x7f) {
		uint32_t mask = (c <= 0x00efbfbf) ? 0x000f0000 : 0x003f0000;
		c = ((c & 0x07000000) >> 6) |
			((c & mask)       >> 4) |
			((c & 0x00003f00) >> 2) |
			 (c & 0x0000003f);
	}
	return c;
}
/* END code from https://dev.to/rdentato/utf-8-strings-in-c-2-3-3kp1 */

static void keypress(TickitKeyEventInfo *key, const char *seq) {
	char buffer[MAX_STR];
	ssize_t bytes;

	TFrame *selected = ((mwin.type == TERM || mwin.type == TBAR) ? mwin.tframe : sel);

	if (!selected)
		return;

	for (TFrame *tframe = (selected->groupedfocus ? nextvisible(tframes) : selected);
			tframe;
			tframe = (selected->groupedfocus ? nextvisible(tframe->next) : NULL)) {
		if (is_content_visible(tframe) && (tframe == selected || tframe->groupedfocus) && !tframe->readonly) {
			if (seq != NULL) {
				bytes = strlen(seq);
				strcpy(buffer, seq);
			} else {
				if (key->type == TICKIT_KEYEV_TEXT) {
					uint32_t codepoint = str_to_codepoint(key->str);
					vterm_keyboard_unichar(tframe->vt, codepoint, (VTermModifier)key->mod);
				} else { // TICKIT_KEYEV_KEY
					VTermKey k = strp_key(key->str);
					if (k == VTERM_KEY_NONE) {
						/* strip key->str */
						char *r = strrchr(key->str, '-');
						if (r == &key->str[strlen(key->str)] - 1) {
							//DEBUG_LOGF("Ukt", "keypress strp_key(%s) = %d, r[0] = %s", key->str, k, r);
							vterm_keyboard_unichar(tframe->vt, r[0], (VTermModifier)key->mod);
						} else if (r) {
							//DEBUG_LOGF("Ukt", "keypress strp_key(%s) = %d, r[1] = %s", key->str, k, r+1);
							vterm_keyboard_unichar(tframe->vt, r[1], (VTermModifier)key->mod);
						}
					} else {
						//DEBUG_LOGF("Ukt", "keypress strp_key(%s) = %d", key->str, k);
						vterm_keyboard_key(tframe->vt, k, (VTermModifier)key->mod);
					}
				}

				if (tframe->sb_offset != 0)
					vscroll_delta(tframe, -tframe->sb_offset);
				bytes = vterm_output_read(tframe->vt, buffer, sizeof(buffer));
			}

			pty_write(tframe->controller_ptyfd, buffer, bytes);
		}
		if (!selected->groupedfocus)
			break;
	}
}

static void keysequence(char *args[]) {
	if (!args || !args[0])
		return;

	char buffer[MAX_STR + 1];
	int len = strlen(args[0]);

	for (int i = 0; i < len; i += MAX_STR) {
		strncpy(buffer, args[0] + i, MAX_STR);
		buffer[MAX_STR] = '\0';
		keypress(NULL, buffer);
	}
}

static int key_rootwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	TickitKeyEventInfo *key = _info;
	//DEBUG_LOGF("Ukt", "key_rootwin type %d, mod %d, str \"%s\"", key->type, key->mod, key->str);

	curkeyscpy(key->str);
	tickit_window_expose(sbar.win, NULL);

	KeyBinding *binding = NULL;
	if ((binding = keybinding(curkeys, curkeys_index))) {
		unsigned int key_length = MAX_KEYS;
		while (key_length > 1 && strlen(binding->keys[key_length-1]) == 0)
			key_length--;
		if (curkeys_index == key_length) {
			binding->action.cmd(binding->action.args);
			curkeys_index = 0;
			memset(curkeys, 0, sizeof(curkeys));
		}
	} else {
		curkeys_index = 0;
		memset(curkeys, 0, sizeof(curkeys));
		keypress(key, NULL);
	}

	return 1;
}

static int mouse_rootwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	TickitMouseEventInfo *m = _info;
	MWin *mw;
	DEBUG_LOGF("Umt", "mouse_rootwin type %d, button %d, mod %d, line %d, col %d", m->type, m->button, m->mod, m->line, m->col);

	if ((mw = get_mwin_by_coord(m->line, m->col))) {
		memcpy(&mwin, mw, sizeof(MWin));
		free(mw);
		//DEBUG_LOGF("Umt", "mwin.type = %d, mwin.tframe = %p", mwin.type, mwin.tframe);
	}
	curkeymouse(m);
	//for (unsigned int i = 0; i < curkeys_index; i++)
		//DEBUG_LOGF("Umt", "curkeys[%d] = %s", curkeys_index, curkeys[i]);

	KeyBinding *binding = NULL;
	if ((binding = mousebinding(curkeys, curkeys_index))) {
		unsigned int key_length = MAX_KEYS;
		while (key_length > 1 && strlen(binding->keys[key_length-1]) == 0)
			key_length--;
		if (curkeys_index == key_length) {
			binding->action.cmd(binding->action.args);
			curkeys_index = 0;
			memset(curkeys, 0, sizeof(curkeys));
			mwin.type = NONE;
		}
	} else {
		curkeys_index = 0;
		memset(curkeys, 0, sizeof(curkeys));
		mwin.type = NONE;
		//mousepress(m);
	}

	tickit_window_expose(sbar.win, NULL);

	return 1;
}

static int sigint_rootwin(Tickit *t, TickitEventFlags flags, void *info, void *data) {
	keypress(&(TickitKeyEventInfo) {
			.type = TICKIT_KEYEV_TEXT,
			.mod = TICKIT_MOD_CTRL,
			.str = "c",
			}, NULL);
	return 1;
}

static int sigwinch_rootwin(Tickit *t, TickitEventFlags flags, void *info, void *data) {
	tickit_term_setctl_int(root.tt, TICKIT_TERMCTL_MOUSE, TICKIT_TERM_MOUSEMODE_OFF);
	tickit_term_setctl_int(root.tt, TICKIT_TERMCTL_MOUSE, TICKIT_TERM_MOUSEMODE_DRAG);
	tickit_term_clear(root.tt);
	arrange();
	return 1;
}

static void create_rootwin(void) {
	root.tickit = tickit_new_stdio();

	root.win = tickit_get_rootwin(root.tickit);
	if (!root.win) {
		fprintf(stderr, "Cannot create TickitTerm - %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	tickit_window_bind_event(root.win, TICKIT_WINDOW_ON_GEOMCHANGE, 0, &resize_rootwin, NULL);
	tickit_window_bind_event(root.win, TICKIT_WINDOW_ON_KEY, 0, &key_rootwin, NULL);
	tickit_window_bind_event(root.win, TICKIT_WINDOW_ON_MOUSE, 0, &mouse_rootwin, NULL);

	/* pass Ctrl-C SIGINT to sel */
	tickit_watch_signal(root.tickit, SIGINT, TICKIT_BIND_FIRST, &sigint_rootwin, NULL);
	/* If size has not changed then refresh */
	tickit_watch_signal(root.tickit, SIGWINCH, TICKIT_BIND_FIRST, &sigwinch_rootwin, NULL);

	root.rect = tickit_window_get_geometry(root.win);

	root.tt = tickit_get_term(root.tickit);
	tickit_term_set_utf8(root.tt, true);
	tickit_term_setctl_str(root.tt, TICKIT_TERMCTL_ICONTITLE_TEXT, application_name);

	const char *colorterm = getenv("COLORTERM");
	if (colorterm && (strstr(colorterm, "truecolor") || strstr(colorterm, "24bit")))
		tickit_term_setctl_int(root.tt, tickit_termctl_lookup("xterm.cap_rgb8"), 1);
}

static void destroy_rootwin(void) {
	tickit_window_close(root.win);
	tickit_unref(root.tickit);
}

static int render_sbarwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	TickitExposeEventInfo *info = _info;
	TickitRenderBuffer *rb = info->rb;
	unsigned int occupied = 0, urgent = 0;
	int l, c, width, maxwidth;

	if (!sbar.showbar)
		return 1;

	for (TFrame *tframe = tframes; tframe; tframe = tframe->next) {
		occupied |= tframe->tags;
		if (tframe->urgent)
			urgent |= tframe->tags;
	}

	int right = sbar.rect.cols - 1;
	int bottom = sbar.rect.lines - 1;

	tickit_renderbuffer_eraserect(rb, &(TickitRect){
		.top = 0, .left = 0, .lines = sbar.rect.lines, .cols = sbar.rect.cols,
	});


	tickit_renderbuffer_setpen(rb, config.statusbar_pen);
	tickit_renderbuffer_hline_at(rb, 0, 0, right, TICKIT_LINE_SINGLE, 0);
	tickit_renderbuffer_vline_at(rb, 0, bottom, right, TICKIT_LINE_SINGLE, 0);
	tickit_renderbuffer_vline_at(rb, 0, bottom, 0, TICKIT_LINE_SINGLE, 0);
	tickit_renderbuffer_goto(rb, 0, 0);
	for (unsigned int i = 0; i < config.ntags; i++) {
		if (tagset[seltags] & (1 << i))
			tickit_renderbuffer_setpen(rb, config.tag_selected_pen);
		else if (urgent & (1 << i))
			tickit_renderbuffer_setpen(rb, config.tag_urgent_pen);
		else if (occupied & (1 << i))
			tickit_renderbuffer_setpen(rb, config.tag_occupied_pen);
		else
			tickit_renderbuffer_setpen(rb, config.tag_unoccupied_pen);
		config.taglens[i] = tickit_renderbuffer_textf(rb, config.tag_printf, config.tagnames[i]);
	}

	tickit_renderbuffer_setpen(rb, (GROUPEDFOCUS ? config.tag_selected_pen : config.tag_unoccupied_pen));
	layoutsymlen = tickit_renderbuffer_textf(rb, "%s", currlayout->symbol);
	tickit_renderbuffer_setpen(rb, config.tag_unoccupied_pen);

	for (unsigned int i = 0; i < curkeys_index; i++) {
		if (strncmp(curkeys[i], "C-", 2) == 0 && strlen(curkeys[i]) == 3)
			tickit_renderbuffer_textf(rb, "^%c", curkeys[i][2] - 32); // uppercase the control char
		else
			tickit_renderbuffer_textf(rb, "%s", curkeys[i]);
	}

	tickit_renderbuffer_textf(rb, "%c", config.statusbar_begin);

	tickit_renderbuffer_get_cursorpos(rb, &l, &c);
	maxwidth = sbar.rect.cols - c - 1;
	wchar_t wbuf[sizeof(sbar.text)];
	size_t numchars = mbstowcs(wbuf, sbar.text, sizeof(sbar.text));
	if (numchars != (size_t)-1 && (width = wcswidth(wbuf, maxwidth)) != -1) {
		tickit_renderbuffer_setpen(rb, config.statusbar_pen);
		if (width < maxwidth) {
			tickit_renderbuffer_textf(rb, "%*c%ls", maxwidth - width, ' ', wbuf);
		} else {
			int pos = 0;
			for (size_t i = 0; i < numchars; i++) {
				pos += wcwidth(wbuf[i]);
				if (pos > maxwidth)
					break;
				tickit_renderbuffer_textf(rb, "%lc", wbuf[i]);
			}
		}
	}

	tickit_renderbuffer_goto(rb, 0, sbar.rect.cols - 1);
	tickit_renderbuffer_setpen(rb, config.statusbar_pen);

	tickit_renderbuffer_setpen(rb, config.tag_unoccupied_pen);
	tickit_renderbuffer_textf(rb, "%c", config.statusbar_end);

	return 1;
}

static int resize_sbarwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	//TickitGeomchangeEventInfo *info = _info;
	//DEBUG_LOGF("Urt", "resize_sbarwin top = %d, left = %d, lines = %d, cols = %d", info->rect.top, info->rect.left, info->rect.lines, info->rect.cols);
	tickit_window_expose(sbar.win, NULL);
	return 1;
}

static int get_status(Tickit *t, TickitEventFlags flags, void *_info, void *data) {
	if (!config.sbar_cmds)
		return 1;

	FILE *fp = popen(config.sbar_cmds[sbar_cmd_num], "r");
	if (fp == NULL) {
		error("Failed to run statusbar command \"%s\"", config.sbar_cmds[sbar_cmd_num]);
		return 0;
	}
	int len = sizeof(sbar.text);
	char *ptr = sbar.text;
	while (fgets(ptr, len, fp) && len > 1) {
		ptr += strcspn(ptr, "\n");
		ptr[0] = 0;
		len = sizeof(sbar.text) - strlen(sbar.text);
	}
	pclose(fp);

	tickit_window_expose(sbar.win, NULL);

	/* if statusbar_interval is 0 then run once and don't reschedule */
	if (config.statusbar_interval > 0)
		sbar.watch_cmd = tickit_watch_timer_after_msec(root.tickit, config.statusbar_interval * 1000, 0, &get_status, NULL);

	sbar_cmd_num = (sbar_cmd_num + 1) % config.nsbar_cmds;

	return 1;
}

static void statusbar(char *args[]) {
	if (!args || !args[0])
		return;
	else if (ARGS0EQ("next")) {
		if (sbar.watch_cmd)
			tickit_watch_cancel(root.tickit, sbar.watch_cmd);
		/* if statusbar_interval is <0 or no command then disable */
		if (config.statusbar_interval >= 0 && config.nsbar_cmds > 0)
			tickit_watch_later(root.tickit, 0, &get_status, NULL);
	} else if ((ARGS0EQ("vis")) ||
			(ARGS0EQ("on") && !sbar.showbar) ||
			(ARGS0EQ("off") && sbar.showbar)) {
		sbar.showbar = pertag.showbar[pertag.curtag] = !sbar.showbar;
		arrange();
	} else if ((ARGS0EQ("pos")) ||
			(ARGS0EQ("top") && !sbar.topbar) ||
			(ARGS0EQ("bottom") && sbar.topbar)) {
		sbar.topbar = pertag.topbar[pertag.curtag] = !sbar.topbar;
		arrange();
	}
}

static void create_sbarwin(void) {
	sbar.showbar = true;
	sbar.topbar = true;

	sbar.win = tickit_window_new(root.win, root.rect, 0);
	sbar.bind_expose = tickit_window_bind_event(sbar.win, TICKIT_WINDOW_ON_EXPOSE, 0, &render_sbarwin, NULL);
	sbar.bind_geomchange = tickit_window_bind_event(sbar.win, TICKIT_WINDOW_ON_GEOMCHANGE, 0, &resize_sbarwin, NULL);

	set_sbar_rect();

	char *args[] = {"next"};
	statusbar(args);
}

static void destroy_sbarwin(void) {
	tickit_window_unbind_event_id(sbar.win, sbar.bind_expose);
	tickit_window_unbind_event_id(sbar.win, sbar.bind_geomchange);
	tickit_window_close(sbar.win);
	tickit_window_unref(sbar.win);
}

static void draw_frameline(TickitRenderBuffer *rb, FrameLine *l) {
	//DEBUG_LOGF("Udf", "draw_frameline, top = %d, left = %d, lines = %d, cols = %d, char = %d", l->line.top, l->line.left, l->line.lines, l->line.cols, l->c);
	if (l->line.lines == 1 && l->line.cols == 1) {  /* intersection character */
		tickit_renderbuffer_char_at(rb, l->line.top, l->line.left, l->c);
	} else if (l->line.lines == 1) {                /* horizontal line */
		tickit_renderbuffer_hline_at(rb,
				l->line.top, l->line.left, l->line.left + l->line.cols - 1,
				TICKIT_LINE_SINGLE, TICKIT_LINECAP_BOTH);
	} else if (l->line.cols == 1) {                 /* vertical line */
		tickit_renderbuffer_vline_at(rb,
				l->line.top, l->line.top + l->line.lines - 1, l->line.left,
				TICKIT_LINE_SINGLE, TICKIT_LINECAP_BOTH);
	}
}

static void add_frameline(TickitRect *line, wchar_t c) {
	FrameLine *newline = malloc(sizeof(FrameLine));
	newline->line = *line;
	newline->c = c;
	newline->next = NULL;

	if (frame.lines) {
		FrameLine *l;
		for (l = frame.lines; l->next; l = l->next);
		l->next = newline;
	} else {
		frame.lines = newline;
	}
}

static void add_frame_vline(int top, int left, int lines) {
	TickitRect line = { top, left, lines, 1 };
	add_frameline(&line, FRAME_VLINE);
}

static void add_frame_char(int top, int left, wchar_t c) {
	TickitRect line = { top, left, 1, 1 };
	add_frameline(&line, c);
}

static void clear_framelines(void) {
	FrameLine *l;
	while (frame.lines) {
		l = frame.lines->next;
		free(frame.lines);
		frame.lines = l;
	}
	tickit_window_expose(frame.win, NULL);
}

static int render_framewin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	TickitExposeEventInfo *info = _info;
	TickitRenderBuffer *rb = info->rb;

	tickit_renderbuffer_eraserect(rb, &(TickitRect){
		.top = 0, .left = 0, .lines = frame.rect.lines, .cols = frame.rect.cols,
	});

	tickit_renderbuffer_setpen(rb, config.unselected_pen);
	for (FrameLine *l = frame.lines; l; l = l->next)
		draw_frameline(rb, l);

	return 1;
}

static int resize_framewin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	//TickitGeomchangeEventInfo *info = _info;
	//DEBUG_LOGF("Urt", "resize_framewin top = %d, left = %d, lines = %d, cols = %d", info->rect.top, info->rect.left, info->rect.lines, info->rect.cols);
	tickit_window_expose(frame.win, NULL);
	return 1;
}

static void create_framewin(void) {
	frame.win = tickit_window_new(root.win, root.rect, 0);
	frame.bind_expose = tickit_window_bind_event(frame.win, TICKIT_WINDOW_ON_EXPOSE, 0, &render_framewin, NULL);
	frame.bind_geomchange = tickit_window_bind_event(frame.win, TICKIT_WINDOW_ON_GEOMCHANGE, 0, &resize_framewin, NULL);

	set_frame_rect();
}

static void destroy_framewin(void) {
	clear_framelines();
	tickit_window_unbind_event_id(frame.win, frame.bind_expose);
	tickit_window_unbind_event_id(frame.win, frame.bind_geomchange);
	tickit_window_close(frame.win);
	tickit_window_unref(frame.win);
}

static void resize_term(TFrame *tframe, TickitRect rect) {
	DEBUG_LOGF("Urt", "resize_term tframe = %p, top = %d, left = %d, lines = %d, cols = %d", tframe, rect.top, rect.left, rect.lines, rect.cols);
	tickit_window_set_geometry(tframe->win, rect);
	tickit_window_show(tframe->win);
}

static void erase(void) {
	clear_framelines();
	for(TFrame *tframe = tframes; tframe; tframe = tframe->next)
		tickit_window_hide(tframe->win);
}

static bool isvisible(TFrame *tframe) {
	return (tframe->tags & tagset[seltags]);
}

static bool is_content_visible(TFrame *tframe) {
	if (!tframe)
		return false;
	if (isarrange(fullscreen))
		return sel == tframe;
	return isvisible(tframe) && !tframe->minimized;
}

static TFrame *nextvisible(TFrame *tframe) {
	for (; tframe && !isvisible(tframe); tframe = tframe->next);
	return tframe;
}

static void set_sbar_rect(void) {
	sbar.rect.top = sbar.topbar ? 0 : root.rect.lines - 1;
	sbar.rect.left = 0;
	sbar.rect.lines = 1;
	sbar.rect.cols = root.rect.cols;
	tickit_window_set_geometry(sbar.win, sbar.rect);
}

static void set_frame_rect(void) {
	frame.rect.top = (sbar.showbar && sbar.topbar) ? 1 : 0;
	frame.rect.left = 0;
	frame.rect.lines = root.rect.lines - (sbar.showbar ? 1 : 0);
	frame.rect.cols = root.rect.cols;
	tickit_window_set_geometry(frame.win, frame.rect);
}

static void arrange(void) {
	unsigned int m = 0, n = 0;
	DEBUG_LOGF("Uar", "arrange()");
	for (TFrame *tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
		tframe->order = ++n;
		if (tframe->minimized)
			m++;
	}
	erase();
	set_sbar_rect();
	set_frame_rect();
	currlayout->arrange();

	if (m && !isarrange(fullscreen)) {
		unsigned int i = 0;
		TickitRect r;
		r.top = frame.rect.lines - 1;
		r.left = 0;
		r.lines = 1;
		r.cols = frame.rect.cols / m;
		for (TFrame *tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
			if (tframe->minimized) {
				if (++i == m)
					r.cols = frame.rect.cols - r.left;
				tickit_window_set_geometry(tframe->win, r);
				tickit_window_show(tframe->win);
				r.left += r.cols;
			}
		}
	}

	dofocus(NULL);
	tickit_window_expose(sbar.win, NULL);
}

static void redraw(char *args[]) {
	/* Handle ^L in a4 application */
	arrange();

	/* Pass ^L through to selected termwins
	keypress(&(TickitKeyEventInfo) {
			.type = TICKIT_KEYEV_TEXT,
			.mod = TICKIT_MOD_CTRL,
			.str = "l",
			}, NULL);
	*/
}

static void layout(char *args[]) {
	if (!args || !args[0])
		return;

	char *layoutname = args[0];

	if (!strcmp(layoutname, "swap")) {
		if (pertag.lastlayout[pertag.curtag])
			layoutname = pertag.lastlayout[pertag.curtag];
		else
			layoutname = "+1";
	}

	pertag.lastlayout[pertag.curtag] = currlayout->name;
	if (!strcmp(layoutname, "+1") || !strcmp(layoutname, "1")) {
		if (currlayout++ == &config.layouts[config.nlayouts - 1])
			currlayout = &config.layouts[0];
	} else if (!strcmp(layoutname, "-1")) {
		if (currlayout-- == &config.layouts[0])
			currlayout = &config.layouts[config.nlayouts - 1];
	} else {
		unsigned int i;
		for (i = 0; i < config.nlayouts; i++)
			if (!strcmp(layoutname, config.layouts[i].name))
				break;
		if (i == config.nlayouts)
			return;
		currlayout = &config.layouts[i];
	}
	pertag.currlayout[pertag.curtag] = currlayout;
	arrange();
}

static int resize_tframewin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	TickitGeomchangeEventInfo *info = _info;
	TFrame *tframe = data;
	DEBUG_LOGF("Urt", "resize_tframewin, %d/%d/%d/%d", info->rect.top, info->rect.left, info->rect.lines, info->rect.cols);

	tframe->rect = info->rect;

	TickitRect tbar_rect = { 0, 0, 1, tframe->rect.cols };
	tickit_window_set_geometry(tframe->tbar, tbar_rect);
	tickit_window_expose(tframe->tbar, NULL);

	TickitRect termwin_rect = { 1, 0, tframe->rect.lines - 1, tframe->rect.cols };
	tickit_window_set_geometry(tframe->termwin, termwin_rect);
	tickit_window_expose(tframe->termwin, NULL);

	return 1;
}

static void create_tframe(void) {
	TFrame *tframe = calloc(1, sizeof(TFrame));
	tframe->tags = tagset[seltags];

	tframe->rect.top = 0;
	tframe->rect.left = 0;
	tframe->rect.lines = frame.rect.lines;
	tframe->rect.cols = frame.rect.cols;

	tframe->win = tickit_window_new(frame.win, tframe->rect, 0);
	tframe->bind_geomchange = tickit_window_bind_event(tframe->win, TICKIT_WINDOW_ON_GEOMCHANGE, 0, &resize_tframewin, tframe);

	create_tbarwin(tframe);
	create_termwin(tframe);

	// set and apply initial default colors
	applycolorrules(tframe);

	attach(tframe);
	dofocus(tframe);
}

static void attachafter(TFrame *tframe, TFrame *a) { /* attach tframe after a */
	if (tframe == a)
		return;
	if (!a)
		for (a = tframes; a && a->next; a = a->next);

	if (a) {
		if (a->next)
			a->next->prev = tframe;
		tframe->next = a->next;
		tframe->prev = a;
		a->next = tframe;
		for (int o = a->order; tframe; tframe = nextvisible(tframe->next))
			tframe->order = ++o;
	}
}

static void attachstack(TFrame *tframe) {
	tframe->snext = stack;
	stack = tframe;
}

static void destroy_tframe(TFrame *tframe) {
	if (sel == tframe)
		focusnextvis();
	detach(tframe);
	detachstack(tframe);
	if (sel == tframe) {
		TFrame *next = nextvisible(tframes);
		if (next) {
			dofocus(next);
			minimize(NULL);
		} else {
			sel = NULL;
		}
	}
	if (lastsel == tframe)
		lastsel = NULL;
	destroy_termwin(tframe);
	destroy_tbarwin(tframe);

	tickit_window_unbind_event_id(tframe->win, tframe->bind_geomchange);
	tickit_window_close(tframe->win);
	tickit_window_unref(tframe->win);
	free(tframe);

	if(!tframes)
		quit(NULL);
	else {
		tagschanged();
	}
}

static void detachstack(TFrame *tframe) {
	TFrame **stackptr;
	for (stackptr = &stack; *stackptr && *stackptr != tframe; stackptr = &(*stackptr)->snext);
	*stackptr = tframe->snext;
}

static void dofocus(TFrame *tframe) {
	if (!tframe)
		for (tframe = stack; tframe && !isvisible(tframe); tframe = tframe->snext);
	if(sel == tframe) {
		if (tframe && tframe->termwin)
			tickit_window_take_focus(tframe->termwin);
		return;
	}
	lastsel = sel;
	sel = tframe;
	if (lastsel) {
		lastsel->urgent = false;
		if (sel && (!isarrange(fullscreen))) {
			if (lastsel->groupedfocus != sel->groupedfocus) {
				expose_all_tbars();
			} else {
				tickit_window_expose(lastsel->tbar, NULL);
			}
		}
	}

	if (tframe) {
		detachstack(tframe);
		attachstack(tframe);
		tframe->urgent = false;
		tickit_window_raise_to_front(tframe->win);
		tickit_window_take_focus(tframe->termwin);
		tickit_term_setctl_str(root.tt, TICKIT_TERMCTL_ICONTITLE_TEXT, tframe->title);
	} else
		tickit_term_setctl_str(root.tt, TICKIT_TERMCTL_ICONTITLE_TEXT, application_name);
}

static void focusnextvis(void) {
	char *args[] = {"NEXT"};
	focus(args);
}

static void focus(char *args[]) {
	TFrame *tframe;

	if ((mwin.type == TERM || mwin.type == TBAR) && (!args || !args[0])) {
		dofocus(mwin.tframe);
		if (mwin.tframe->minimized)
			minimize(NULL);
	} else if (!sel || !args || !args[0]) {  /* no sel or missing arg */
		return;
	} else if (ARGS0EQ("left")) {
		TFrame *tframe = get_tframe_by_coord(frame.rect.top + sel->rect.top, sel->rect.left - 2);
		if (tframe)
			dofocus(tframe);
	} else if (ARGS0EQ("right")) {
		TFrame *tframe = get_tframe_by_coord(frame.rect.top + sel->rect.top, sel->rect.left + sel->rect.cols + 1);
		if (tframe)
			dofocus(tframe);
	} else if (ARGS0EQ("up")) {
		TFrame *tframe = get_tframe_by_coord(frame.rect.top + sel->rect.top - 1, sel->rect.left);
		if (tframe)
			dofocus(tframe);
	} else if (ARGS0EQ("down")) {
		TFrame *tframe = get_tframe_by_coord(frame.rect.top + sel->rect.top + sel->rect.lines, sel->rect.left);
		if (tframe)
			dofocus(tframe);
	} else if (ARGS0EQ("next")) {
		for (tframe = sel->next; tframe && !isvisible(tframe); tframe = tframe->next);
		if(!tframe)
			for (tframe = tframes; tframe && !isvisible(tframe); tframe = tframe->next);
		if (tframe)
			dofocus(tframe);
	} else if (ARGS0EQ("prev")) {
		for (tframe = sel->prev; tframe && !isvisible(tframe); tframe = tframe->prev);
		if (!tframe) {
			for (tframe = tframes; tframe && tframe->next; tframe = tframe->next);
			for (; tframe && !isvisible(tframe); tframe = tframe->prev);
		}
		if (tframe)
			dofocus(tframe);
	} else if (ARGS0EQ("NEXT")) {
		tframe = sel;
		do {
			tframe = nextvisible(tframe->next);
			if (!tframe)
				tframe = nextvisible(tframes);
		} while (tframe->minimized && tframe != sel);
		dofocus(tframe);
	} else if (ARGS0EQ("PREV")) {
		tframe = sel;
		do {
			for (tframe = tframe->prev; tframe && !isvisible(tframe); tframe = tframe->prev);
			if (!tframe) {
				for (tframe = tframes; tframe && tframe->next; tframe = tframe->next);
				for (; tframe && !isvisible(tframe); tframe = tframe->prev);
			}
		} while (tframe && tframe != sel && tframe->minimized);
		dofocus(tframe);
	} else if (ARGS0EQ("swap")) {
		if (lastsel)
			dofocus(lastsel);
	} else if (ARGS0EQ("group")) {
		if (mwin.type == TERM || mwin.type == TBAR)
			dofocus(mwin.tframe);
		sel->groupedfocus = !sel->groupedfocus;
		expose_all_tbars();
	} else if (ARGS0EQ("groupall") || ARGS0EQ("0")) {
		sel->groupedfocus = !sel->groupedfocus;
		for (TFrame *tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
			tframe->groupedfocus = sel->groupedfocus;
			tickit_window_expose(tframe->tbar, NULL);
		}
		expose_all_tbars();
	} else {
		for (TFrame *tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
			if (tframe->order == atol(args[0])) {
				dofocus(tframe);
				if (tframe->minimized)
					minimize(NULL);
				return;
			}
		}
	}
}

static void zoom(char *args[]) {
	TFrame *tframe;

	if ((mwin.type == TERM || mwin.type == TBAR) && (!args || !args[0]))
		dofocus(mwin.tframe);
	else if (!sel)
		return;
	else if (args && args[0] && (atol(args[0]) > 0))
		focus(args);

	if ((tframe = sel) == nextvisible(tframes))
		if (!(tframe = nextvisible(tframe->next)))
			return;
	detach(tframe);
	attach(tframe);
	dofocus(tframe);
	if (tframe->minimized)
		minimize(NULL);
	arrange();
}

/* returns the bit corresponding to the tagname in config.tagnames[] */
static unsigned int bitoftag(const char *tag) {
	unsigned int i;
	if (!tag)
		return ~0;
	for (i = 0; (i < config.ntags) && strcmp(config.tagnames[i], tag); i++);
	return (i < config.ntags) ? (1 << i) : 0;
}

static void tagschanged() {
	bool allminimized = true;
	for (TFrame *tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next)) {
		if (!tframe->minimized) {
			allminimized = false;
			break;
		}
	}
	if (allminimized && nextvisible(tframes)) {
		dofocus(NULL);
		minimize(NULL);
	}
	arrange();
}

static void tag(char *args[]) {
	if (!sel)
		return;

	if (mwin.type == TAG)                  /* mouse on tag name */
		sel->tags = bitoftag(config.tagnames[mwin.tag]) & TAGMASK;
	else if (!args || !args[0])            /* missing arg */
		return;
	else if (ARGS0EQ("_all"))              /* special arg "_all" */
		sel->tags = bitoftag(NULL) & TAGMASK;
	else
		sel->tags = bitoftag(args[0]) & TAGMASK;

	tagschanged();
}

static void tagtoggle(char *args[]) {
	if (!sel)
		return;

	unsigned int newtags;
	if (mwin.type == TAG)                  /* mouse on tag name */
		newtags = sel->tags ^ (bitoftag(config.tagnames[mwin.tag]) & TAGMASK);
	else if (!args || !args[0])            /* missing arg */
		return;
	else
		newtags = sel->tags ^ (bitoftag(args[0]) & TAGMASK);

	if (newtags) {
		sel->tags = newtags;
		tagschanged();
	}
}

static void viewswap(void) {
	seltags ^= 1;
	unsigned int tmptag = pertag.prevtag;
	pertag.prevtag = pertag.curtag;
	pertag.curtag = tmptag;
	set_pertag();
	tagschanged();
}

static void viewset(char *tagname) {
	unsigned int newtagset = bitoftag(tagname) & TAGMASK;
	if (newtagset && tagset[seltags] != newtagset) {
		seltags ^= 1; /* toggle sel tagset */
		pertag.prevtag = pertag.curtag;
		if(tagname != NULL) {
			unsigned int i;
			for (i = 0; (i < config.ntags) && (strcmp(config.tagnames[i], tagname) != 0); i++) ;
			pertag.curtag = i + 1;
		}
		set_pertag();
		tagset[seltags] = newtagset;
		tagschanged();
	}
}

static void view(char *args[]) {
	if (mwin.type == TAG)                  /* mouse on tag name */
		viewset(config.tagnames[mwin.tag]);
	else if (!args || !args[0])            /* missing arg */
		return;
	else if (ARGS0EQ("_all"))              /* special arg "_all" */
		viewset(NULL);
	else if (ARGS0EQ("_swap"))             /* special arg "_swap" */
		viewswap();
	else
		viewset(args[0]);
}

static void viewtoggle(char *args[]) {
	char *tagname;
	if (mwin.type == TAG)                  /* mouse on tag name */
		tagname = config.tagnames[mwin.tag];
	else if (!args || !args[0])            /* missing arg */
		return;
	else
		tagname = args[0];

	unsigned int newtagset = tagset[seltags] ^ (bitoftag(tagname) & TAGMASK);
	if (newtagset) {
		if(newtagset == TAGMASK) {
			pertag.prevtag = pertag.curtag;
			pertag.curtag = 0;
		} else if(!(newtagset & 1 << (pertag.curtag - 1))) {
			pertag.prevtag = pertag.curtag;
			int i;
			for (i=0; !(newtagset &1 << i); i++) ;
			pertag.curtag = i + 1;
		}
		set_pertag();
		tagset[seltags] = newtagset;
		tagschanged();
	}
}

static void readonly(char *args[]) {
	if ((mwin.type == TERM || mwin.type == TBAR) && (!args || !args[0]))
		dofocus(mwin.tframe);
	else if (!sel)
		return;

	sel->readonly = !sel->readonly;
	tickit_window_expose(sel->tbar, NULL);
}

static void minimize(char *args[]) {
	TFrame *tframe, *m, *t;
	TFrame *savesel = NULL;
	unsigned int n;

	if (mwin.type == TERM || mwin.type == TBAR) {
		savesel = (mwin.tframe->minimized ? mwin.tframe : sel);
		dofocus(mwin.tframe);
	} else if (mwin.type || !sel)
		return;

	/* the last window can't be minimized */
	if (!sel->minimized) {
		for (n = 0, tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next))
			if (!tframe->minimized)
				n++;
		if (n == 1)
			return;
	}

	sel->minimized = !sel->minimized;
	m = sel;
	/* check whether the zoomed client was minimized */
	if (sel == nextvisible(tframes) && sel->minimized) {
		tframe = nextvisible(sel->next);
		detach(tframe);
		attach(tframe);
		dofocus(tframe);
		detach(m);
		for (; tframe && (t = nextvisible(tframe->next)) && !t->minimized; tframe = t);
		attachafter(m, tframe);
	} else if (m->minimized) {
		/* non zoomed window got minimized move it above all other minimized ones */
		focusnextvis();
		detach(m);
		for (tframe = nextvisible(tframes); tframe && (t = nextvisible(tframe->next)) && !t->minimized; tframe = t);
		attachafter(m, tframe);
	} else { /* window is no longer minimized, move it to the zoomed area */
		detach(m);
		attach(m);
	}
	arrange();
	if (savesel)
		dofocus(savesel);
}

static void expose_all_tbars(void) {
	for (TFrame *tframe = nextvisible(tframes); tframe; tframe = nextvisible(tframe->next))
		tickit_window_expose(tframe->tbar, NULL);
}

static int render_tbarwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	TickitExposeEventInfo *info = _info;
	TickitRenderBuffer *rb = info->rb;
	TFrame *tframe = data;
	TickitPen *pen = config.unselected_pen;
	int maxlen;

	int right = tframe->rect.left + tframe->rect.cols - 1;

	tickit_renderbuffer_eraserect(rb, &(TickitRect){
		.top = 0, .left = 0, .lines = 1, .cols = tframe->rect.cols,
	});

	if (tframe->readonly && sel != tframe && tframe->urgent && (GROUPEDFOCUS && tframe->groupedfocus && !tframe->minimized))
		pen = config.ro_urg_selected_pen;
	else if (tframe->readonly && sel != tframe && tframe->urgent)
		pen = config.ro_urg_unselected_pen;
	else if (tframe->readonly && (sel == tframe || (GROUPEDFOCUS && tframe->groupedfocus && !tframe->minimized)))
		pen = config.ro_selected_pen;
	else if (tframe->readonly)
		pen = config.ro_unselected_pen;
	else if (sel != tframe && tframe->urgent && (GROUPEDFOCUS && tframe->groupedfocus && !tframe->minimized))
		pen = config.urg_selected_pen;
	else if (sel != tframe && tframe->urgent)
		pen = config.urg_unselected_pen;
	else if (sel == tframe || (GROUPEDFOCUS  && tframe->groupedfocus && !tframe->minimized))
		pen = config.selected_pen;

	tickit_renderbuffer_setpen(rb, pen);
	tickit_renderbuffer_hline_at(rb, 0, 0, right, TICKIT_LINE_SINGLE, TICKIT_LINECAP_BOTH);

	maxlen = tframe->rect.cols - 9;
	if (maxlen < 0)
		maxlen = 0;

	if (sel != tframe && tframe->groupedfocus && !tframe->minimized) {
		if (tframe->readonly && tframe->urgent)
			tickit_renderbuffer_setpen(rb, config.ro_urg_selected_pen);
		else if (tframe->readonly)
			tickit_renderbuffer_setpen(rb, config.ro_selected_pen);
		else if (tframe->urgent)
			tickit_renderbuffer_setpen(rb, config.urg_selected_pen);
		else
			tickit_renderbuffer_setpen(rb, config.selected_pen);
	}
	tickit_renderbuffer_textf_at(rb, 0, 1, "[%.*s%s#%d]",
								maxlen, *tframe->title ? tframe->title : "",
								*tframe->title ? " | " : "",
								tframe->order);

	return 1;
}

static int resize_tbarwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	//TickitGeomchangeEventInfo *info = _info;
	//DEBUG_LOGF("Urt", "resize_tbarwin top = %d, left = %d, lines = %d, cols = %d", info->rect.top, info->rect.left, info->rect.lines, info->rect.cols);
	return 1;
}

static void create_tbarwin(TFrame *tframe) {
	TickitRect rect = { 0, 0, 1, tframe->rect.cols };
	tframe->tbar = tickit_window_new(tframe->win, rect, 0);
	tframe->tbar_bind_expose = tickit_window_bind_event(tframe->tbar, TICKIT_WINDOW_ON_EXPOSE, 0, &render_tbarwin, tframe);
	tframe->tbar_bind_geomchange = tickit_window_bind_event(tframe->tbar, TICKIT_WINDOW_ON_GEOMCHANGE, 0, &resize_tbarwin, NULL);
}

static void destroy_tbarwin(TFrame *tframe) {
	tickit_window_unbind_event_id(tframe->tbar, tframe->tbar_bind_expose);
	tickit_window_unbind_event_id(tframe->tbar, tframe->tbar_bind_geomchange);
	tickit_window_close(tframe->tbar);
	tickit_window_unref(tframe->tbar);
}

static int render_termwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	TickitExposeEventInfo *info = _info;
	TickitRenderBuffer *rb = info->rb;
	TickitRect rect = info->rect;
	TFrame *tframe = data;
	VTermPos ppos, vpos;
	VTermScreenCell cell, lastcell;

	TickitPen *pen = tickit_pen_new();

	DEBUG_LOGF("Urt", "render_termwin rect = %d/%d/%d/%d, tframe = %p", rect.top, rect.left, rect.lines, rect.cols, tframe);
	tickit_renderbuffer_eraserect(rb, &rect);

	for (ppos.row = rect.top; ppos.row < (rect.top + rect.lines); ppos.row++) {
		for (ppos.col = rect.left; ppos.col < (rect.left + rect.cols); ) {
			vpos.row = ppos.row - tframe->sb_offset;
			vpos.col = ppos.col;
			fetch_cell(tframe, vpos, &cell);
			if ((vpos.col == rect.left) || !compare_cells(&cell, &lastcell)) {
				/* set pen */
				TickitPen_from_VTermScreenCell(pen, &cell, tframe->cs);
				tickit_renderbuffer_setpen(rb, pen);
				lastcell = cell;
			}

			if (cell.chars[0] == 0) {
				//DEBUG_LOGF("Urt", "render_termwin, empty cell fg/bg type = %d/%d,
				//		default = %d/%d, idx = %d/%d, rgb = %02X:%02X:%02X/%02X:%02X:%02X",
				//		cell.fg.type & VTERM_COLOR_TYPE_MASK, cell.bg.type & VTERM_COLOR_TYPE_MASK,
				//		cell.fg.type & VTERM_COLOR_DEFAULT_MASK, cell.bg.type & VTERM_COLOR_DEFAULT_MASK,
				//		cell.fg.indexed.idx, cell.bg.indexed.idx,
				//		cell.fg.rgb.red, cell.fg.rgb.green, cell.fg.rgb.blue,
				//		cell.bg.rgb.red, cell.bg.rgb.green, cell.bg.rgb.blue);
				if (!vterm_screen_is_eol(tframe->vts, ppos)) {
					tickit_renderbuffer_erase_at(rb, ppos.row, ppos.col, 1);
				} else {
					tickit_renderbuffer_erase_at(rb, ppos.row, ppos.col, tframe->rect.cols - ppos.col);
					break;
				}
			} else {
				for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) {
					char bytes[6];
					bytes[fill_utf8(cell.chars[i], bytes)] = 0;
					tickit_renderbuffer_textf_at(rb, ppos.row, ppos.col, "%s", bytes);
				}
			}

			ppos.col += cell.width;
		}
	}

	tickit_pen_unref(pen);

	return 1;
}

static int resize_termwin(TickitWindow *win, TickitEventFlags flags, void *_info, void *data) {
	TickitGeomchangeEventInfo *info = _info;
	TickitRect rect = info->rect;
	TFrame *tframe = data;
	tframe->termrect.lines = MAX(rect.lines, 2); // FIXME: why does this require 2 and not 1?
	tframe->termrect.cols = MAX(rect.cols, 2);   // FIXME: Test if this is required or not.
	DEBUG_LOGF("Urt", "resize_termwin, lines = %d, cols = %d", tframe->termrect.lines, tframe->termrect.cols);

	/* Set size of pty terminal */
	struct winsize ws = { tframe->termrect.lines, tframe->termrect.cols, 0, 0 };
	ioctl(tframe->controller_ptyfd, TIOCSWINSZ, &ws);

	/* Set size of vterm */
	vterm_set_size(tframe->vt, tframe->termrect.lines, tframe->termrect.cols);
	vterm_screen_flush_damage(tframe->vts);

	return 1;
}

static void create_termwin(TFrame *tframe) {
	TickitRect rect = { 1, 0, tframe->rect.lines - 1, tframe->rect.cols };
	tframe->termwin = tickit_window_new(tframe->win, rect, 0);
	tframe->termwin_bind_expose = tickit_window_bind_event(tframe->termwin, TICKIT_WINDOW_ON_EXPOSE, 0, &render_termwin, tframe);
	tframe->termwin_bind_geomchange = tickit_window_bind_event(tframe->termwin, TICKIT_WINDOW_ON_GEOMCHANGE, 0, &resize_termwin, tframe);

	tickit_window_set_cursor_visible(tframe->termwin, config.cursorvis);
	tickit_window_set_cursor_shape(tframe->termwin, config.cursorshape);
	tickit_window_setctl_int(tframe->termwin, TICKIT_WINCTL_CURSORBLINK, config.cursorblink);

	tframe->termrect = rect;
	tframe->termrect.top = 0;

	get_vterm(tframe);
}

static void destroy_termwin(TFrame *tframe) {
	free_vterm(tframe);
	tickit_window_unbind_event_id(tframe->termwin, tframe->termwin_bind_expose);
	tickit_window_unbind_event_id(tframe->termwin, tframe->termwin_bind_geomchange);
	tickit_window_close(tframe->termwin);
	tickit_window_unref(tframe->termwin);
}

static void attach(TFrame *tframe) {
	if (tframes)
		tframes->prev = tframe;
	tframe->next = tframes;
	tframe->prev = NULL;
	tframes = tframe;
	for (int o = 1; tframe; tframe = nextvisible(tframe->next), o++)
		tframe->order = o;
}

static void detach(TFrame *tframe) {
	TFrame *d;
	if (tframe->prev)
		tframe->prev->next = tframe->next;
	if (tframe->next) {
		tframe->next->prev = tframe->prev;
		for (d = nextvisible(tframe->next); d; d = nextvisible(d->next))
			--d->order;
	}
	if (tframe == tframes)
		tframes = tframe->next;
	tframe->next = tframe->prev = NULL;
}

static void noaction(char *args[]) {
	/* map to this to unbind a KeyCombo */
}

static void create(char *args[]) {
	create_tframe();
	arrange();
}

static void destroy(char *args[]) {
	TFrame *selected = (mwin.type == TERM || mwin.type == TBAR ? mwin.tframe : sel);
	if (!selected || selected->readonly)
		return;
	kill(-selected->worker_pid, SIGKILL);
}

bool isarrange(void (*func)()) {
	return func == currlayout->arrange;
}

static bool hasstackarea(void) {
	if (isarrange(fullscreen) || isarrange(grid) ||
		isarrange(columns) || isarrange(rows))
		return false;
	return true;
}

static void zoomnum(char *args[]) {
	int delta;

	if (!hasstackarea())
		return;

	if (!args || !args[0]) {
		currzoomnum = config.zoomnum;
	} else if (sscanf(args[0], "%d", &delta) == 1) {
		if (args[0][0] == '+' || args[0][0] == '-')
			currzoomnum += delta;
		else
			currzoomnum = delta;
		if (currzoomnum < 1)
			currzoomnum = 1;
	}
	pertag.currzoomnum[pertag.curtag] = currzoomnum;
	arrange();
}

static void zoomsize(char *args[]) {
	float delta;

	if (!hasstackarea())
		return;

	if (!args || !args[0]) {
		currzoomsize = config.zoomsize;
	} else if (sscanf(args[0], "%f", &delta) == 1) {
		if (args[0][0] == '+' || args[0][0] == '-')
			currzoomsize += delta;
		else
			currzoomsize = delta;
		if (currzoomsize < 0.1)
			currzoomsize = 0.1;
		else if (currzoomsize > 0.9)
			currzoomsize = 0.9;
	}
	pertag.currzoomsize[pertag.curtag] = currzoomsize;
	arrange();
}

static void vscroll_delta(TFrame *tframe, int delta) {
	// FIXME: Deal with altscreen
	//DEBUG_LOGF("Usb", "vscroll_delta %d", delta);

	if (delta > 0) {
		if (tframe->sb_offset + delta > tframe->sb_current)
			delta = tframe->sb_current - tframe->sb_offset;
	} else if (delta < 0) {
		if(delta < -tframe->sb_offset)
			delta = -tframe->sb_offset;
	}

	if(!delta)
		return;

	tframe->sb_offset += delta;

	tickit_window_expose(tframe->termwin, NULL);
	tickit_window_set_cursor_visible(tframe->termwin, config.cursorvis && !tframe->sb_offset);
}

static void scrollback(char *args[]) {
	/* If scrollback argument has a decimal point it specifies the percentage of a
	 * screen of lines to scroll, otherwise it is the whole number of lines to
	 * scroll. Positive numbers scroll back and negative numbers scroll forward. */
	if (!args[0])
		return;

	int delta = 0;
	float pct = 0;
	if (strchr(args[0], '.'))
		pct = atof(args[0]);
	else
		delta = atoi(args[0]);

	TFrame *scrollsel = (mwin.type == TERM ? mwin.tframe : sel);
	for (TFrame *tframe = ((scrollsel && scrollsel->groupedfocus) ? nextvisible(tframes) : scrollsel);
			tframe;
			tframe = nextvisible(tframe->next)) {
		if (is_content_visible(tframe) && (tframe == scrollsel || tframe->groupedfocus))
			vscroll_delta(tframe, (pct ? tframe->termrect.lines * pct : delta));
		if (!(scrollsel && scrollsel->groupedfocus))
			break;
	}

}

static void set_pertag(void) {
	currlayout = pertag.currlayout[pertag.curtag];
	currzoomnum = pertag.currzoomnum[pertag.curtag];
	currzoomsize = pertag.currzoomsize[pertag.curtag];
	sbar.showbar = pertag.showbar[pertag.curtag];
	sbar.topbar = pertag.topbar[pertag.curtag];
}

static void destroy_pertag(void) {
	free(pertag.currzoomsize);
	free(pertag.currzoomnum);
	free(pertag.lastlayout);
	free(pertag.currlayout);
	free(pertag.showbar);
	free(pertag.topbar);
}

static void create_pertag(void) {
	pertag.curtag = pertag.prevtag = 1;

	pertag.currlayout = calloc(config.ntags+1, sizeof(*pertag.currlayout));
	pertag.lastlayout = calloc(config.ntags+1, sizeof(*pertag.lastlayout));
	pertag.currzoomnum = calloc(config.ntags+1, sizeof(*pertag.currzoomnum));
	pertag.currzoomsize = calloc(config.ntags+1, sizeof(*pertag.currzoomsize));
	pertag.showbar = calloc(config.ntags+1, sizeof(*pertag.showbar));
	pertag.topbar = calloc(config.ntags+1, sizeof(*pertag.topbar));

	for(unsigned int i = 0; i <= config.ntags; i++) {
		pertag.currlayout[i] = config.layouts;
		pertag.lastlayout[i] = NULL;
		pertag.currzoomnum[i] = config.zoomnum;
		pertag.currzoomsize[i] = config.zoomsize;
		pertag.showbar[i] = config.statusbar_display;
		pertag.topbar[i] = config.statusbar_top;
	}

	set_pertag();
}

static void startup_a4(void) {
	setlocale(LC_CTYPE, "");
	parse_config(&config);
	shell = getshell(application_name);

	create_rootwin();
	create_sbarwin();
	create_framewin();

	create_pertag();

	if (startups)
		for (unsigned int i = 0; i < config.nstartups; i++)
			config.startups[i].action.cmd(config.startups[i].action.args);
}

static void shutdown_a4(void) {
	while (tframes)
		destroy_tframe(tframes);

	destroy_pertag();

	destroy_sbarwin();
	destroy_framewin();
	destroy_rootwin();

	destroy_config(&config);
}

static void quit(char *args[]) {
	running = false;
}

static void usage(const char *errstr, ...) {
	if (errstr) {
		fprintf(stderr, "ERROR: ");
		va_list ap;
		va_start(ap, errstr);
		vfprintf(stderr, errstr, ap);
		va_end(ap);
		fprintf(stderr, "\n");
	}
	fprintf(stderr, "usage: %s [-h|-?] [-i file] [-s] [-v]\n", application_name);
	exit(EXIT_FAILURE);
}

static void parse_args(int argc, char *argv[]) {
	const char *name = argv[0];

	if (name && (name = strrchr(name, '/')))
		application_name = name + 1;
	for (int arg = 1; arg < argc; arg++) {
		if (argv[arg][0] != '-' || argv[arg][1] == '\0') {
			usage("Invalid option %s", argv[arg]);
		}
		while (argv[arg][1] != '\0') {
			if (argv[arg][1] == 'i') {
				if ((arg + 1) >= argc || argv[arg][2] != '\0')
					usage("Missing argument to -i");
				a4configfname = argv[++arg];
				break;
			} else {
				switch (argv[arg][1]) {
					case 'h':
					case '?':
						usage(NULL);
						break;
					case 's':
						startups = false;
						break;
					case 'v':
						fprintf(stderr, "%s %s © 2022-2023 Ross P. Mohn\n", application_name, VERSION);
						exit(EXIT_SUCCESS);
					default:
						usage("Invalid option -%c", argv[arg][1]);
				}
				strcpy(argv[arg]+1, argv[arg]+2);
			}
		}
	}
}

int main(int argc, char *argv[]) {
#ifndef NDEBUG
	tickit_debug_init();
#endif
	/* FIXME: should be able to run a4 inside of a4, but crashes */
	if (getenv("A4"))
		usage("Cannot run a4 inside of a4");
	setenv("A4", VERSION, 1);
	parse_args(argc, argv);
	startup_a4();

	/* putting tickit_tick in my own loop avoids the SIGINT capture in tickit_run */
	while (running) {
		for (TFrame *tframe = tframes; tframe;) {
			if (tframe->died) {
				TFrame *t = tframe->next;
				destroy_tframe(tframe);
				tframe = t;
				break; // Destroy tframes one at a time so that tickit_tick can perform clean up
			}

			/* check each termwin for vterm_output_read()
			 * these are not directly related to a keypress */
			char buffer[MAX_STR];
			ssize_t bytes;
			bytes = vterm_output_read(tframe->vt, buffer, sizeof(buffer));
			if (bytes)
				pty_write(tframe->controller_ptyfd, buffer, bytes);

			tframe = tframe->next;
		}

		tickit_tick(root.tickit, 0);
	}

	shutdown_a4();
	return 0;
}
