#ifdef __GLIBC__
#  define _XOPEN_SOURCE 500  /* strdup */
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <tickit.h>

/* Matches the a4 default dbl_click_ms */
#define DBL_CLICK_MS 400

TickitTerm *tt;
char curkey[256];
int line = 0;

static int multi_click = 0; /* 0 = none, 2 = double pending, 3 = triple pending */
static struct {
	int button, line, col;
	struct timespec time;
} last_press;

/* Mirror a4's multi-click detection so dbl-/tpl- names are discoverable */
static void detect_multi_click(TickitMouseEventInfo *m) {
	if (m->type != TICKIT_MOUSEEV_PRESS)
		return;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long elapsed_ms = (now.tv_sec - last_press.time.tv_sec) * 1000 +
	                  (now.tv_nsec - last_press.time.tv_nsec) / 1000000;
	bool same_spot = m->button == last_press.button &&
	                 m->line == last_press.line && m->col == last_press.col;
	bool in_time = (last_press.time.tv_sec || last_press.time.tv_nsec) &&
	               elapsed_ms < DBL_CLICK_MS;
	if (multi_click == 2 && same_spot && in_time) {
		multi_click = 3;
		last_press.time = (struct timespec){0, 0};
	} else if (multi_click != 2 && same_spot && in_time) {
		multi_click = 2;
		last_press.time = now;
	} else {
		multi_click = 0;
		last_press.button = m->button;
		last_press.line = m->line;
		last_press.col = m->col;
		last_press.time = now;
	}
}

static void curkey_mouse(TickitMouseEventInfo *m) {
	char *ck = curkey;

	detect_multi_click(m);

	ck += sprintf(ck, "%s%s%s",
			(m->mod & TICKIT_MOD_ALT ? "M-" : ""),
			(m->mod & TICKIT_MOD_CTRL ? "C-" : ""),
			(m->mod & TICKIT_MOD_SHIFT ? "S-" : ""));

	switch(m->type) {
		case TICKIT_MOUSEEV_WHEEL:
			sprintf(ck, "wheel-%s",
					m->button == TICKIT_MOUSEWHEEL_UP   ? "up" :
					m->button == TICKIT_MOUSEWHEEL_DOWN ? "dn" :
					m->button == TICKIT_MOUSEWHEEL_LEFT ? "left" : "right");
			break;
		case TICKIT_MOUSEEV_PRESS:
			sprintf(ck, multi_click == 3 ? "tpl-press-%d" :
			            multi_click == 2 ? "dbl-press-%d" : "press-%d", m->button);
			break;
		case TICKIT_MOUSEEV_DRAG:
			sprintf(ck, multi_click == 3 ? "tpl-drag-%d" :
			            multi_click == 2 ? "dbl-drag-%d" : "drag-%d", m->button);
			break;
		case TICKIT_MOUSEEV_RELEASE:
			sprintf(ck, multi_click == 3 ? "tpl-release-%d" :
			            multi_click == 2 ? "dbl-release-%d" : "release-%d", m->button);
			/* Keep multi_click == 2 so a third press can promote to triple;
			 * a completed triple ends the cycle. */
			if (multi_click == 3)
				multi_click = 0;
			break;
		case TICKIT_MOUSEEV_DRAG_START:
		case TICKIT_MOUSEEV_DRAG_STOP:
		case TICKIT_MOUSEEV_DRAG_DROP:
		case TICKIT_MOUSEEV_DRAG_OUTSIDE:
			break;
	}
}

static void curkey_keyboard(const char *str) {
	strcpy(curkey, str);

	int lastchar = strlen(str) - 1;
	switch(str[lastchar]) {
		case ' ':
			strcpy(curkey + lastchar, "Space");
			break;
		case '-':
			strcpy(curkey + lastchar, "Hyphen");
			break;
	}
}

static int event_key(TickitWindow *win, TickitEventFlags flags, void *_info, void *data)
{
	TickitKeyEventInfo *info = _info;
	curkey_keyboard(info->str);
	tickit_window_expose(win, NULL);
	return 1;
}

static int event_mouse(TickitWindow *win, TickitEventFlags flags, void *_info, void *data)
{
	TickitMouseEventInfo *info = _info;
	curkey_mouse(info);
	tickit_window_expose(win, NULL);
	return 1;
}

static int render_root(TickitWindow *win, TickitEventFlags flags, void *_info, void *data)
{
	TickitExposeEventInfo *info = _info;
	TickitRect rect = info->rect;
	TickitRenderBuffer *rb = info->rb;

	if (strlen(curkey) == 0)
		return 1;

	if (line >= rect.lines) {
		tickit_term_scrollrect(tt, rect, rect.lines - line + 1, 0);
		line = rect.lines - 1;
	}
	tickit_renderbuffer_textf_at(rb, line++, 0, "KEYCODE: %s", curkey);
	curkey[0] = '\0';

	return 1;
}

static int event_resize(TickitWindow *root, TickitEventFlags flags, void *_info, void *data)
{
	tickit_window_expose(root, NULL);
	return 1;
}

int main(int argc, char *argv[])
{
	Tickit *t = tickit_new_stdtty();
	tickit_setctl_int(t, TICKIT_CTL_USE_ALTSCREEN, 0);
	tt = tickit_get_term(t);

	TickitWindow *root = tickit_get_rootwin(t);
	if(!root) {
		fprintf(stderr, "Cannot create TickitTerm - %s\n", strerror(errno));
		return 1;
	}

	tickit_window_bind_event(root, TICKIT_WINDOW_ON_KEY, 0, &event_key, NULL);
	tickit_window_bind_event(root, TICKIT_WINDOW_ON_MOUSE, 0, &event_mouse, NULL);
	tickit_window_bind_event(root, TICKIT_WINDOW_ON_EXPOSE, 0, &render_root, NULL);
	tickit_window_bind_event(root, TICKIT_WINDOW_ON_GEOMCHANGE, 0, &event_resize, NULL);

	tickit_window_take_focus(root);
	tickit_window_set_cursor_visible(root, false);

	tickit_run(t);

	tickit_window_close(root);
	tickit_unref(t);

	return 0;
}
