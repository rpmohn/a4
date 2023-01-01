#ifdef __GLIBC__
#  define _XOPEN_SOURCE 500  /* strdup */
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <tickit.h>

TickitTerm *tt;
char curkey[256];
unsigned int line = 0;

static void curkey_mouse(TickitMouseEventInfo *m) {
	char *ck = curkey;

	ck += sprintf(ck, "%s%s%s",
			(m->mod & TICKIT_MOD_ALT ? "M-" : ""),
			(m->mod & TICKIT_MOD_CTRL ? "C-" : ""),
			(m->mod & TICKIT_MOD_SHIFT ? "S-" : ""));

	switch(m->type) {
		case TICKIT_MOUSEEV_WHEEL:
			sprintf(ck, "wheel-%s", (m->button == TICKIT_MOUSEWHEEL_UP ? "up" : "dn"));
			break;
		case TICKIT_MOUSEEV_PRESS:
			sprintf(ck, "press-%d", m->button);
			break;
		case TICKIT_MOUSEEV_DRAG:
			sprintf(ck, "drag-%d", m->button);
			break;
		case TICKIT_MOUSEEV_RELEASE:
			sprintf(ck, "release-%d", m->button);
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
