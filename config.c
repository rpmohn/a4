#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>
#include <strings.h>
#include <stdlib.h>

#include "lib/ini.h"

#define LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))
#define DELIMS " 	"

/* typedefs */
typedef struct {
	TickitPen *selected_pen;
	TickitPen *unselected_pen;
	TickitPen *urg_selected_pen;
	TickitPen *urg_unselected_pen;
	TickitPen *ro_selected_pen;
	TickitPen *ro_unselected_pen;
	TickitPen *ro_urg_selected_pen;
	TickitPen *ro_urg_unselected_pen;

	unsigned int nsbar_cmds;
	char **sbar_cmds;
	int statusbar_interval;
	TickitPen *statusbar_pen;
	bool statusbar_display;
	bool statusbar_top;
	char statusbar_begin;
	char statusbar_end;

	unsigned int ntags;
	char **tagnames;
	char *tag_printf;
	int *taglens;
	TickitPen *tag_selected_pen;
	TickitPen *tag_occupied_pen;
	TickitPen *tag_unoccupied_pen;
	TickitPen *tag_urgent_pen;

	bool cursorvis;
	bool cursorblink;
	int cursorshape;

	float zoomsize;
	unsigned int zoomnum;
	int scroll_history;

	unsigned int nstartups;
	KeyBinding *startups;
	unsigned int nkb_bindings;
	KeyBinding *kb_bindings;
	unsigned int nmterm_bindings;
	KeyBinding *mterm_bindings;
	unsigned int nmtbar_bindings;
	KeyBinding *mtbar_bindings;
	unsigned int nmsbar_bindings;
	KeyBinding *msbar_bindings;
	unsigned int nmtag_bindings;
	KeyBinding *mtag_bindings;
	unsigned int nmlayout_bindings;
	KeyBinding *mlayout_bindings;
	unsigned int nmframe_bindings;
	KeyBinding *mframe_bindings;
	unsigned int nlayouts;
	Layout *layouts;
	unsigned int ncolorschemes;
	ColorScheme *colorschemes;
	unsigned int ncolorrules;
	ColorRule *colorrules;
} Config;

typedef struct {
	char *name;
	char *value;
} Pair;

/* function declarations */
static char *get_colorvalue(Pair *colornames, const char *key);
static int rgb_to_idx(const int r, const int g, const int b);
static void hex_to_tickit_pen(TickitPen *pen, TickitPenAttr attr, int32_t num);
static void set_attrs_color(TickitPen *pen, TickitPenAttr attr, const char *value);
static void set_attrs_style(TickitPen *pen, const char *value);
static void set_attrs(TickitPen *pen, const char *value);
static void build_urg_pens(Config *cfg, const char *value);
static void build_ro_pens(Config *cfg, const char *value);
static void destroy_tags(Config *cfg);
static void create_tags(Config *cfg, const char *value);
static void prepare_str(char **dst, const char *src);
static void destroy_layouts(Config *cfg);
static void create_layout(Config *cfg, const char *name, const char *value);
static void build_vtermcolor(const char *value, VTermColor *color);
static void clear_colorschemes(Config *cfg);
static void destroy_colorschemes(Config *cfg);
static ColorScheme *create_colorscheme(Config *cfg, const char *name);
static ColorScheme *get_colorscheme(Config *cfg, const char *name);
static void update_colorscheme(Config *cfg, const char *section, const char *name, const char *value);
static void clear_colorrules(Config *cfg);
static void destroy_colorrules(Config *cfg);
static void create_colorrule(Config *cfg, const char *name, const char *value);
static void clear_sbar_cmds(Config *cfg);
static void destroy_sbar_cmds(Config *cfg);
static void create_sbar_cmd(Config *cfg, const char *value);
static char *interpret_backslashes(const char *value);
static void clear_bindings(unsigned int *nbindings, KeyBinding **bindings);
static void destroy_bindings(unsigned int *nbindings, KeyBinding **bindings);
static void create_binding(unsigned int *nbindings, KeyBinding **bindings, const char *name, const char *value);
static void create_startup(unsigned int *nbindings, KeyBinding **bindings, const char *name, const char *value);
static void expand_num_keybinding_action(Config *cfg, const char *kstr, const char *astr);
static void expand_tag_keybinding_action(Config *cfg, const char *kstr, const char *astr);
static bool special_keyword(Config *cfg, const char *name, const char *value);
static int a4_ini_handler(void *user, const char *section, const char *name, const char *value);
static char *buildpath(char *dst, const char *src1, const char *src2, const char *src3);
static bool file_exists(char *fname);
static char *getconfigfname(char *found, char *search);
static void include_config(Config *cfg, char *fname);
static void destroy_config(Config *cfg);
static void preset_configs(Config *cfg);
static void postset_configs(Config *cfg);
static int parse_config(Config *cfg);

/* globals */
static struct {
	char *name;
	void (*func)(char *args[]);
} action_choices[] = {
	ACTIONS(XCHOOSE)
};

static struct {
	char *name;
	void (*func)(void);
} layout_choices[] = {
	LAYOUTS(XCHOOSE)
};

static char *a4configfnameptr;
static Config config;
static Pair colornames[] = {
#include "lib/rgb.inc"
	};

/* functions */
static char *get_colorvalue(Pair *colornames, const char *key) {
	for (int i = 0; colornames[i].name; i++)
		if (!strcasecmp(key, colornames[i].name))
			return colornames[i].value;
	return NULL;
}

static int rgb_to_idx(const int r, const int g, const int b) {
	/* see lookup_colour_palette() in libvterm/src/pen.c */
	/* good enough grayscale ramp black to white: 16, 232-255, 231 */
	if (r == g && g == b) {
		if (r < 0x07)
			return 16;  /* black 0x000000 */
		if (r > 0xF5)
			return 231; /* white 0xFFFFFF */

		return 232 + ((r - 6) / 10); /* truncation through integer division */
	}
	/* good enough 6 x 6 x 6 RGB color cube: 16-231 */
	return 16 + (36 * (r / 51)) + (6 * (g / 51)) + (b / 51); /* truncation through integer division */
}

static void hex_to_tickit_pen(TickitPen *pen, TickitPenAttr attr, int32_t num) {
	unsigned int r, g, b, n = num;
	r = n >> 16 & 0xFF;
	g = n >>  8 & 0xFF;
	b = n >>  0 & 0xFF;
	tickit_pen_set_colour_attr(pen, attr, rgb_to_idx(r, g, b));
	tickit_pen_set_colour_attr_rgb8(pen, attr, (TickitPenRGB8){ .r = r, .g = g, .b = b });
}

static void set_attrs_color(TickitPen *pen, TickitPenAttr attr, const char *value) {
	char *endptr;
	int32_t num = strtol(value, &endptr, 0);

	if (endptr == value) {
		char *c = get_colorvalue(colornames, value);
		if (c) {                                    // Use colorname
			num = strtol(c, NULL, 0);
			hex_to_tickit_pen(pen, attr, num);
		} else
			error("Invalid color value %s in configuration file", value);

	} else if (strncasecmp(value, "0x", 2) == 0) {  // 0xRRGGBB color
		hex_to_tickit_pen(pen, attr, num);

	} else if (num == -1) {                         // Set to default color
		tickit_pen_clear_attr(pen, attr);

	} else if (num >= 0 && num < 256) {             // 0-255 indexed color
		tickit_pen_set_colour_attr(pen, attr, num);

	} else {                                        // error
		error("Invalid color value %s in configuration file", value);
	}
}

static void set_attrs_style(TickitPen *pen, const char *value) {
	char *v, *attr, *save;

	v = strdup(value);
	for (attr = strtok_r(v, ", 	", &save);
			attr != NULL;
			attr = strtok_r(NULL, ", 	", &save)) {
		if (strcasecmp(attr, "bold") == 0)
			tickit_pen_set_bool_attr(pen, TICKIT_PEN_BOLD, true);
		else if (strcasecmp(attr, "italic") == 0)
			tickit_pen_set_bool_attr(pen, TICKIT_PEN_ITALIC, true);
		else if (strcasecmp(attr, "single") == 0)
			tickit_pen_set_int_attr(pen, TICKIT_PEN_UNDER, TICKIT_PEN_UNDER_SINGLE);
		else if (strcasecmp(attr, "double") == 0)
			tickit_pen_set_int_attr(pen, TICKIT_PEN_UNDER, TICKIT_PEN_UNDER_DOUBLE);
		else if (strcasecmp(attr, "curly") == 0)
			tickit_pen_set_int_attr(pen, TICKIT_PEN_UNDER, TICKIT_PEN_UNDER_WAVY);
		else if (strcasecmp(attr, "blink") == 0)
			tickit_pen_set_bool_attr(pen, TICKIT_PEN_BLINK, true);
		else if (strcasecmp(attr, "reverse") == 0)
			tickit_pen_set_bool_attr(pen, TICKIT_PEN_REVERSE, true);
		else if (strcasecmp(attr, "strike") == 0)
			tickit_pen_set_bool_attr(pen, TICKIT_PEN_STRIKE, true);
	}
	free(v);
}

static void set_attrs(TickitPen *pen, const char *value) {
	unsigned int i;
	char *str, *tok, *save;

	save = NULL;
	str = strdup(value);
	for (i = 0, tok = strtok_r(str, DELIMS, &save);
			tok != NULL;
			i++, tok = strtok_r(NULL, DELIMS, &save)) {
		switch (i) {
			case 0:
				set_attrs_color(pen, TICKIT_PEN_FG, tok);
				break;
			case 1:
				set_attrs_color(pen, TICKIT_PEN_BG, tok);
				break;
			case 2:
				set_attrs_style(pen, tok);
				break;
		}
	}
	free(str);
}

static void build_urg_pens(Config *cfg, const char *value) {
	unsigned int i;
	char *str, *tok, *save;

	if (cfg->urg_selected_pen)
		tickit_pen_unref(cfg->urg_selected_pen);
	cfg->urg_selected_pen = tickit_pen_clone(cfg->selected_pen);
	if (cfg->urg_unselected_pen)
		tickit_pen_unref(cfg->urg_unselected_pen);
	cfg->urg_unselected_pen = tickit_pen_clone(cfg->unselected_pen);

	save = NULL;
	str = strdup(value);
	for (i = 0, tok = strtok_r(str, DELIMS, &save);
			tok != NULL;
			i++, tok = strtok_r(NULL, DELIMS, &save)) {
		switch (i) {
			case 0:
				if (strtol(tok, NULL, 0) != -1) {
					set_attrs_color(cfg->urg_selected_pen, TICKIT_PEN_FG, tok);
					set_attrs_color(cfg->urg_unselected_pen, TICKIT_PEN_FG, tok);
				}
				break;
			case 1:
				if (strtol(tok, NULL, 0) != -1) {
					set_attrs_color(cfg->urg_selected_pen, TICKIT_PEN_BG, tok);
					set_attrs_color(cfg->urg_unselected_pen, TICKIT_PEN_BG, tok);
				}
				break;
			case 2:
				set_attrs_style(cfg->urg_selected_pen, tok);
				set_attrs_style(cfg->urg_unselected_pen, tok);
				break;
		}
	}
	free(str);
}

static void build_ro_pens(Config *cfg, const char *value) {
	unsigned int i;
	char *str, *tok, *save;

	if (cfg->ro_selected_pen)
		tickit_pen_unref(cfg->ro_selected_pen);
	cfg->ro_selected_pen = tickit_pen_clone(cfg->selected_pen);
	if (cfg->ro_unselected_pen)
		tickit_pen_unref(cfg->ro_unselected_pen);
	cfg->ro_unselected_pen = tickit_pen_clone(cfg->unselected_pen);
	if (cfg->ro_urg_selected_pen)
		tickit_pen_unref(cfg->ro_urg_selected_pen);
	cfg->ro_urg_selected_pen = tickit_pen_clone(cfg->urg_selected_pen);
	if (cfg->ro_urg_unselected_pen)
		tickit_pen_unref(cfg->ro_urg_unselected_pen);
	cfg->ro_urg_unselected_pen = tickit_pen_clone(cfg->urg_unselected_pen);

	save = NULL;
	str = strdup(value);
	for (i = 0, tok = strtok_r(str, DELIMS, &save);
			tok != NULL;
			i++, tok = strtok_r(NULL, DELIMS, &save)) {
		switch (i) {
			case 0:
				if (strtol(tok, NULL, 0) != -1) {
					set_attrs_color(cfg->ro_selected_pen, TICKIT_PEN_FG, tok);
					set_attrs_color(cfg->ro_unselected_pen, TICKIT_PEN_FG, tok);
					set_attrs_color(cfg->ro_urg_selected_pen, TICKIT_PEN_FG, tok);
					set_attrs_color(cfg->ro_urg_unselected_pen, TICKIT_PEN_FG, tok);
				}
				break;
			case 1:
				if (strtol(tok, NULL, 0) != -1) {
					set_attrs_color(cfg->ro_selected_pen, TICKIT_PEN_BG, tok);
					set_attrs_color(cfg->ro_unselected_pen, TICKIT_PEN_BG, tok);
					set_attrs_color(cfg->ro_urg_selected_pen, TICKIT_PEN_BG, tok);
					set_attrs_color(cfg->ro_urg_unselected_pen, TICKIT_PEN_BG, tok);
				}
				break;
			case 2:
				set_attrs_style(cfg->ro_selected_pen, tok);
				set_attrs_style(cfg->ro_unselected_pen, tok);
				set_attrs_style(cfg->ro_urg_selected_pen, tok);
				set_attrs_style(cfg->ro_urg_unselected_pen, tok);
				break;
		}
	}
	free(str);
}

static void destroy_tags(Config *cfg) {
	while (cfg->ntags > 0) {
		cfg->ntags--;
		if (cfg->tagnames[cfg->ntags])
			free(cfg->tagnames[cfg->ntags]);
	}
	cfg->ntags = 0;
	if (cfg->tagnames)
		free(cfg->tagnames);
	if (cfg->taglens)
		free(cfg->taglens);
}

static void create_tags(Config *cfg, const char *value) {
	char *str, *tok, *save;

	/* Count the number of tagnames listed in value and assign to cfg->ntags */
	save = NULL;
	str = strdup(value);
	for (cfg->ntags = 0, tok = strtok_r(str, DELIMS, &save);
			tok != NULL;
			cfg->ntags++, tok = strtok_r(NULL, DELIMS, &save)) ;
	free(str);

	/* Assign each tag name in value to the array cfg->tagnames */
	cfg->tagnames = calloc(cfg->ntags, sizeof(char*));
	cfg->taglens = calloc(cfg->ntags, sizeof(int));
	save = NULL;
	str = strdup(value);
	for (cfg->ntags = 0, tok = strtok_r(str, DELIMS, &save);
			tok != NULL;
			cfg->ntags++, tok = strtok_r(NULL, DELIMS, &save)) {
		cfg->tagnames[cfg->ntags] = strdup(tok);
	}
	free(str);
}

static void prepare_str(char **dst, const char *src) {
	if (*dst)
		free(*dst);
	*dst = strdup(src + (src[0] == '"' ? 1 : 0));
	if ((*dst)[strlen(*dst) - 1] == '"')
		(*dst)[strlen(*dst) - 1] = '\0';
}

static void destroy_layouts(Config *cfg) {
	for (; cfg->nlayouts > 0; cfg->nlayouts--) {
		free(cfg->layouts[cfg->nlayouts - 1].name);
		free(cfg->layouts[cfg->nlayouts - 1].symbol);
	}
	free(cfg->layouts);
}

static void create_layout(Config *cfg, const char *name, const char *value) {
	unsigned int i;

	for (i = 0; i < LENGTH(layout_choices); i++)
		if (!strcmp(name, layout_choices[i].name))
			break;
	if (i == LENGTH(layout_choices))
		error("Invalid layout function %s in configuration file", name);

	cfg->layouts = realloc(cfg->layouts, ++cfg->nlayouts * sizeof(Layout));
	cfg->layouts[cfg->nlayouts - 1].name = strdup(name);
	cfg->layouts[cfg->nlayouts - 1].symbol = NULL;
	prepare_str(&cfg->layouts[cfg->nlayouts - 1].symbol, value);
	cfg->layouts[cfg->nlayouts - 1].arrange = layout_choices[i].func;
}

static void build_vtermcolor(const char *value, VTermColor *color) {
	char *endptr;
	int32_t num = strtol(value, &endptr, 0);

	if (endptr == value) {
		char *c = get_colorvalue(colornames, value);
		if (c) {                                    // Use colorname
			num = strtol(c, NULL, 0);
			unsigned int n = num;
			vterm_color_rgb(color, n >> 16 & 0xFF, n >>  8 & 0xFF, n >>  0 & 0xFF);
		} else
			error("Invalid color value %s in configuration file", value);

	} else if (strncasecmp(value, "0x", 2) == 0) {  // 0xRRGGBB color
		unsigned int n = num;
		vterm_color_rgb(color, n >> 16 & 0xFF, n >>  8 & 0xFF, n >>  0 & 0xFF);

	} else if (num == -1) {
		return;

	} else if (num >=0 && num < 256) {              // 0-255 indexed color
		vterm_color_indexed(color, num);

	} else {                                        // error
		error("Invalid color value %s in configuration file", value);
	}
}

static void clear_colorschemes(Config *cfg) {
	for (; cfg->ncolorschemes > 0; cfg->ncolorschemes--) {
		free(cfg->colorschemes[cfg->ncolorschemes - 1].name);
		tickit_pen_unref(cfg->colorschemes[cfg->ncolorschemes - 1].pen);
	}
	free(cfg->colorschemes);
	cfg->colorschemes = NULL;
}

static void destroy_colorschemes(Config *cfg) {
	clear_colorschemes(cfg);
	free(cfg->colorschemes);
}

static ColorScheme *create_colorscheme(Config *cfg, const char *name) {
	cfg->colorschemes = realloc(cfg->colorschemes, ++cfg->ncolorschemes * sizeof(ColorScheme));
	cfg->colorschemes[cfg->ncolorschemes - 1].name = strdup(name);

	/* FIXME: set default fg/bg
	cfg->colorschemes[cfg->ncolorschemes - 1].fg = terminal default?
	cfg->colorschemes[cfg->ncolorschemes - 1].bg = terminal default?
	*/

	/* Initialize all color index values */
	for (int i = 0; i < MAX_COLORINDEX; i++) {
		cfg->colorschemes[cfg->ncolorschemes - 1].palette[i].type = VTERM_COLOR_INDEXED;
		cfg->colorschemes[cfg->ncolorschemes - 1].palette[i].indexed.idx = i;
	}

	cfg->colorschemes[cfg->ncolorschemes -1].pen = tickit_pen_new();

	return &cfg->colorschemes[cfg->ncolorschemes - 1];
}

static ColorScheme *get_colorscheme(Config *cfg, const char *name) {
	for (unsigned int i = 0; i < cfg->ncolorschemes; i++)
		if (strcasecmp(name, cfg->colorschemes[i].name) == 0)
			return &cfg->colorschemes[i];

	return NULL;
}

static void update_colorscheme(Config *cfg, const char *section, const char *name, const char *value) {
	// get second word of section as the colorscheme name
	unsigned int i;
	char *str, *tok, *save;
	char *csname;
	ColorScheme *cs;

	if (!strcmp(name, "") || !strcmp(value, "")) {
		clear_colorschemes(cfg);
		return;
	}

	// parse the name and copy to KeyCombo keys
	save = NULL;
	str = strdup(section);
	for (i = 0, tok = strtok_r(str, DELIMS, &save);
			tok != NULL;
			i++, tok = strtok_r(NULL, DELIMS, &save)) {
		if (i == 1) {
			csname = strdup(tok);
			break;
		}
	}
	free(str);

	// FIXME: Deal with defaults. What if no color schemes are defined? What if only some colors are defined?
	if ((cs = get_colorscheme(cfg, csname)) == NULL)
		cs = create_colorscheme(cfg, csname);
	free(csname);

	if (strcasecmp(name, "fg") == 0) {
		build_vtermcolor(value, &cs->fg);
		cs->cell.fg = cs->fg;
		set_attrs_color(cs->pen, TICKIT_PEN_FG, value);
	} else if (strcasecmp(name, "bg") == 0) {
		build_vtermcolor(value, &cs->bg);
		cs->cell.bg = cs->bg;
		set_attrs_color(cs->pen, TICKIT_PEN_BG, value);
	} else if (strcasecmp(name, "cursor") == 0) {
		/* Waiting for function tickit_window_set_cursor_color() */
	} else {
		i = atoi(&name[5]); // FIXME: Needs error checking!
		build_vtermcolor(value, &cs->palette[i]);
	}
}

static void clear_colorrules(Config *cfg) {
	for (; cfg->ncolorrules > 0; cfg->ncolorrules--)
		free(cfg->colorrules[cfg->ncolorrules - 1].title);
}

static void destroy_colorrules(Config *cfg) {
	clear_colorrules(cfg);
	free(cfg->colorrules);
}

static void create_colorrule(Config *cfg, const char *name, const char *value) {
	if (!strcmp(name, "") || !strcmp(value, "")) {
		clear_colorrules(cfg);
		return;
	}
	ColorScheme *palette = get_colorscheme(cfg, value);
	if (!palette)
		error("Invalid ColorScheme %s in configuration file", value);

	// FIXME: Add a default color rule if none are specified
	cfg->colorrules = realloc(cfg->colorrules, ++cfg->ncolorrules * sizeof(ColorRule));
	cfg->colorrules[cfg->ncolorrules - 1].title = strdup(name);
	cfg->colorrules[cfg->ncolorrules - 1].palette = palette;
}

static void clear_sbar_cmds(Config *cfg) {
	while (cfg->nsbar_cmds-- > 0) {
		if (cfg->sbar_cmds[cfg->nsbar_cmds])
			free(cfg->sbar_cmds[cfg->nsbar_cmds]);
	}
	cfg->nsbar_cmds = 0;
}

static void destroy_sbar_cmds(Config *cfg) {
	clear_sbar_cmds(cfg);
	if (cfg->sbar_cmds)
		free(cfg->sbar_cmds);
}

static void create_sbar_cmd(Config *cfg, const char *value) {
	if (!strcmp(value, "")) {
		clear_sbar_cmds(cfg);
		return;
	}
	cfg->sbar_cmds = realloc(cfg->sbar_cmds, (++cfg->nsbar_cmds) * sizeof(char*));
	cfg->sbar_cmds[cfg->nsbar_cmds - 1] = NULL;
	prepare_str(&cfg->sbar_cmds[cfg->nsbar_cmds - 1], value);
}

/* Replace backslash escapes, from echo(1), e.g. '\e' with literal ESC byte \x1B */
/* https://www.geeksforgeeks.org/replace-occurrences-string-ab-c-without-using-extra-space/ */
static char *interpret_backslashes(const char *value) {
	/*
	char num[4];
	char *offset;
	*/

	int l = 0;
	while (isspace((unsigned char) value[l]))
		l++;

	char *str = strdup(value+l);

	if (str[0] == '\0')
		return str;

	for (int i = 1; str[i] != '\0'; i++) {
		if (str[i-1] == '\\') {
			l = 1;
			switch (str[i]) {
				case 'a':				// alert (bell)
					str[i-1] = '\x07';
					break;
				case 'b':				// backspace
					str[i-1] = '\x08';
					break;
				case 'e':				// an escape character
					str[i-1] = '\x1B';
					break;
				case 'f':				// form feed
					str[i-1] = '\x0C';
					break;
				case 'n':				// newline
					str[i-1] = '\x0A';
					break;
				case 'r':				// carriage return
					str[i-1] = '\x0D';
					break;
				case 't':				// horizontal tab
					str[i-1] = '\x09';
					break;
				case 'v':				// vertical tab
					str[i-1] = '\x0B';
					break;
				case '\\':				// backslash
					str[i-1] = '\x5C';
				/* FIXME: implement proper ANSI C backslash numeric escape codes
				case '0':
					strncpy(num, str+i+1, 3);
					num[3] = '\0';
					str[i-1] = strtol(num, &offset, 8);
					l += offset - num;
					break;
				case 'x':
					strncpy(num, str+i+1, 2);
					num[2] = '\0';
					str[i-1] = strtol(num, &offset, 16);
					l += offset - num;
					break;
				*/
			}

			for (int m=0; m < l; m++)
				for (int j=i; str[j] != '\0'; j++)
					str[j] = str[j+1];
		}
	}

	return str;
}

static void clear_bindings(unsigned int *nbindings, KeyBinding **bindings) {
	int i;
	KeyBinding *b = NULL;
	for (; *nbindings > 0; (*nbindings)--) {
		/* keys are copied in and must not be freed             */
		/* args[0] is a pointer to the function                 */
		/* any additional args were strdup'ed and must be freed */
		for (i = 0; i < MAX_ARGS; i++) {
			b = *bindings + *nbindings - 1;
			if (b->action.args[i]) {
				free(b->action.args[i]);
			}
		}
	}
}

static void destroy_bindings(unsigned int *nbindings, KeyBinding **bindings) {
	clear_bindings(nbindings, bindings);
	free(*bindings);
}

static void create_binding(unsigned int *nbindings, KeyBinding **bindings, const char *name, const char *value) {
	unsigned int i, j;
	char *str, *tok, *save;
	char *click;
	KeyBinding *b = NULL;

	*bindings = realloc(*bindings, ++(*nbindings) * sizeof(KeyBinding));
	b = *bindings + *nbindings - 1;
	memset(b, 0, sizeof(KeyBinding));

	// Parse the name and copy to KeyCombo keys. Could be the special keyword "startup"
	save = NULL;
	str = strdup(name);
	for (i = 0, tok = strtok_r(str, DELIMS, &save);
			tok != NULL;
			i++, tok = strtok_r(NULL, DELIMS, &save)) {
		if ((click = strstr(tok, "click-"))) {
			int offset = click - tok;
			sprintf(b->keys[i], "%.*spress%s", offset, tok, tok + offset + 5);
			sprintf(b->keys[++i], "%.*srelease%s", offset, tok, tok + offset + 5);
		} else
			strncpy(b->keys[i], tok, MAX_KEYNAME);
	}
	free(str);

	// If value is blank, map to noaction()
	if (!strcmp(value, "")) {
		b->action.cmd = &noaction;
		return;
	}

	// parse the value and copy to action.cmd, action.args
	save = NULL;
	str = strdup(value);
	for (i = 0, tok = strtok_r(str, DELIMS, &save);
			tok != NULL;
			i++, tok = strtok_r(NULL, DELIMS, &save)) {
		if (i == 0) {
			// look up the action and assign to action.cmd
			for (j = 0; j < LENGTH(action_choices); j++) {
				if (!strcmp(tok, action_choices[j].name)) {
					b->action.cmd = action_choices[j].func;
					break;
				}
			}
			if(j < LENGTH(action_choices) && !strcmp(action_choices[j].name, "keysequence")) {
				b->action.args[0] = interpret_backslashes(value+11);
				break;
			}
		} else {
			// assign arguments to action.cmd
			b->action.args[i - 1] = strdup(tok);
		}
	}
	free(str);

	if (!b->action.cmd)
		error("Invalid KeyBinding Action function \"%s\" in configuration file", value);
}

static void create_startup(unsigned int *nbindings, KeyBinding **bindings, const char *name, const char *value) {
	if (!strcmp(value, ""))
		clear_bindings(nbindings, bindings);
	else
		create_binding(nbindings, bindings, name, value);
}

static void expand_num_keybinding_action(Config *cfg, const char *kstr, const char *astr) {
	char actionstr[MAX_STR];
	char *n = strrchr(kstr, '#');
	if (!n)
		error("KeyBinding \"%s\" must include a '#' character for function \"%s\" to replace in configuration file", kstr, astr);
	for (int i = 1; i <= 9; i++) {
		n[0] = i + 48; // Convert number to character
		sprintf(actionstr, "%s %d", astr, i);
		create_binding(&cfg->nkb_bindings, &cfg->kb_bindings, kstr, actionstr);
	}
}

static void expand_tag_keybinding_action(Config *cfg, const char *kstr, const char *astr) {
	char actionstr[MAX_STR];
	char *n = strrchr(kstr, '#');
	if (!n)
		error("KeyBinding \"%s\" must include a '#' character for function \"%s\" to replace in configuration file", kstr, astr);
	int max = MIN(cfg->ntags, 9);
	for (int i = 0; i < max && cfg->tagnames[i]; i++) {
		n[0] = i + 1 + 48; // Convert number to character
		sprintf(actionstr, "%s %s", astr, cfg->tagnames[i]);
		create_binding(&cfg->nkb_bindings, &cfg->kb_bindings, kstr, actionstr);
	}
}

static bool special_keyword(Config *cfg, const char *name, const char *value) {
	/* Handle special keyword actions */
	if (strcasecmp(value, "focus #") == 0) {
		expand_num_keybinding_action(cfg, name, "focus");
		return true;
	} else if (strcasecmp(value, "view #") == 0) {
		expand_tag_keybinding_action(cfg, name, "view");
		return true;
	} else if (strcasecmp(value, "tag #") == 0) {
		expand_tag_keybinding_action(cfg, name, "tag");
		return true;
	} else if (strcasecmp(value, "viewtoggle #") == 0) {
		expand_tag_keybinding_action(cfg, name, "viewtoggle");
		return true;
	} else if (strcasecmp(value, "tagtoggle #") == 0) {
		expand_tag_keybinding_action(cfg, name, "tagtoggle");
		return true;
	}

	return false;
}

static int a4_ini_handler(void *user, const char *section, const char *name, const char *value) {
	Config *cfg = (Config *)user;

	if (strcasecmp(name, "include") == 0) {
		include_config(cfg, (char *)value);

	} else if (strcasecmp(name, "titlebar_selected") == 0) {
		set_attrs(cfg->selected_pen, value);
	} else if (strcasecmp(name, "titlebar_unselected") == 0) {
		set_attrs(cfg->unselected_pen, value);
	} else if (strcasecmp(name, "titlebar_urgent") == 0) {
		build_urg_pens(cfg, value);
	} else if (strcasecmp(name, "titlebar_readonly") == 0) {
		build_ro_pens(cfg, value);

	} else if (strcasecmp(name, "statusbar_cmd") == 0) {
		create_sbar_cmd(cfg, value);
	} else if (strcasecmp(name, "statusbar_interval") == 0) {
		cfg->statusbar_interval = atoi(value);
	} else if (strcasecmp(name, "statusbar_attr") == 0) {
		set_attrs(cfg->statusbar_pen, value);
	} else if (strcasecmp(name, "statusbar_display") == 0) {
		cfg->statusbar_display = (strcasecmp(value, "true") == 0) ? true : false;
	} else if (strcasecmp(name, "statusbar_top") == 0) {
		cfg->statusbar_top = (strcasecmp(value, "true") == 0) ? true : false;
	} else if (strcasecmp(name, "statusbar_begin") == 0) {
		cfg->statusbar_begin = value[0];
	} else if (strcasecmp(name, "statusbar_end") == 0) {
		cfg->statusbar_end = value[0];

	} else if (strcasecmp(name, "tagnames") == 0) {
		create_tags(cfg, value);
	} else if (strcasecmp(name, "tag_printf") == 0) {
		prepare_str(&cfg->tag_printf, value);
	} else if (strcasecmp(name, "tag_selected") == 0) {
		set_attrs(cfg->tag_selected_pen, value);
	} else if (strcasecmp(name, "tag_occupied") == 0) {
		set_attrs(cfg->tag_occupied_pen, value);
	} else if (strcasecmp(name, "tag_unoccupied") == 0) {
		set_attrs(cfg->tag_unoccupied_pen, value);
	} else if (strcasecmp(name, "tag_urgent") == 0) {
		set_attrs(cfg->tag_urgent_pen, value);

	} else if (strcasecmp(name, "cursorvis") == 0) {
		cfg->cursorvis = (strcasecmp(value, "true") == 0) ? true : false;
	} else if (strcasecmp(name, "cursorblink") == 0) {
		cfg->cursorblink = (strcasecmp(value, "true") == 0) ? true : false;
	} else if (strcasecmp(name, "cursorshape") == 0) {
		cfg->cursorshape = atoi(value);

	} else if (strcasecmp(name, "zoomnum") == 0) {
		cfg->zoomnum = atoi(value);
	} else if (strcasecmp(name, "zoomsize") == 0) {
		cfg->zoomsize = atof(value);
	} else if (strcasecmp(name, "scroll_history") == 0) {
		cfg->scroll_history = atoi(value);

	} else if (strcasecmp(name, "startup") == 0) {
		create_startup(&cfg->nstartups, &cfg->startups, name, value);

	} else if (strcasecmp(section, "keyboardactions") == 0) {
		if (!special_keyword(cfg, name, value))
			create_binding(&cfg->nkb_bindings, &cfg->kb_bindings, name, value);
	} else if (strcasecmp(section, "mousetermwinactions") == 0) {
		create_binding(&cfg->nmterm_bindings, &cfg->mterm_bindings, name, value);
	} else if (strcasecmp(section, "mousetitlebaractions") == 0) {
		create_binding(&cfg->nmtbar_bindings, &cfg->mtbar_bindings, name, value);
	} else if (strcasecmp(section, "mousetagnamesactions") == 0) {
		create_binding(&cfg->nmtag_bindings, &cfg->mtag_bindings, name, value);
	} else if (strcasecmp(section, "mouselayoutSymbolactions") == 0) {
		create_binding(&cfg->nmlayout_bindings, &cfg->mlayout_bindings, name, value);
	} else if (strcasecmp(section, "mousestatustextactions") == 0) {
		create_binding(&cfg->nmsbar_bindings, &cfg->msbar_bindings, name, value);
	} else if (strcasecmp(section, "mouseframelinesactions") == 0) {
		create_binding(&cfg->nmframe_bindings, &cfg->mframe_bindings, name, value);

	} else if (strcasecmp(section, "layouts") == 0) {
		create_layout(cfg, name, value);

	} else if (strncasecmp(section, "colorscheme", 11) == 0) {
		update_colorscheme(cfg, section, name, value);

	} else if (strcasecmp(section, "colorrules") == 0) {
		create_colorrule(cfg, name, value);

	} else {
		return 0;
	}

	return 1;
}

static char *buildpath(char *dst, const char *src1, const char *src2, const char *src3) {
	strncpy(dst, src1, PATH_MAX);
	dst[PATH_MAX - 1] = '\0';
	strncat(dst, src2, PATH_MAX - strlen(dst));
	dst[PATH_MAX - 1] = '\0';
	strncat(dst, src3, PATH_MAX - strlen(dst));
	dst[PATH_MAX - 1] = '\0';
	return dst;
}

static bool file_exists(char *fname) {
	FILE *f = fopen(fname, "r");
	if (f) {
		fclose(f);
		return true;
	}
	return false;
}

static char *getconfigfname(char *found, char *search) {
	const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char configfname[PATH_MAX];

	configfname[0] = '\0';

	/* look in $XDG_CONFIG_HOME/a4/ */
	if (xdg_config_home)
		buildpath(configfname, xdg_config_home, "/a4/", search);

	/* else look in $HOME/.config/a4/ */
	else if (home)
		buildpath(configfname, home, "/.config/a4/", search);

	/* If still no file, look in SYSCONFDIR/a4/ */
	if (configfname[0] == '\0' || !file_exists(configfname))
		buildpath(configfname, SYSCONFDIR, "/a4/", search);

	strcpy(found, configfname);
	//DEBUG_LOGF("Ugc", "getconfigfname returning ::%s::", configfname);
	return found;
}

static void include_config(Config *cfg, char *fname) {
	char includefname[PATH_MAX];
	char *ptr = fname;

	if (fname[0] != '/') {
		char *testpath = strdup(a4configfnameptr);
		char *slash = strrchr(testpath, '/');
		if (slash)
			slash[0] = 0;
		buildpath(includefname, testpath, "/", fname);
		if (!file_exists(includefname))
			getconfigfname(includefname, fname);
		free(testpath);
		ptr = includefname;
	}
	//DEBUG_LOGF("Uic", "include_config ptr = ::%s::", ptr);

	int ret = ini_parse(ptr, a4_ini_handler, cfg);
	if (ret < 0)
		error("Could not load configuration file \"%s\" (%d)", ptr, ret);
	else if (ret > 0)
		error("Error parsing line %d in configuration file \"%s\"", ret, ptr);
}

static void destroy_config(Config *cfg) {
	tickit_pen_unref(cfg->selected_pen);
	tickit_pen_unref(cfg->unselected_pen);
	tickit_pen_unref(cfg->urg_selected_pen);
	tickit_pen_unref(cfg->urg_unselected_pen);
	tickit_pen_unref(cfg->ro_selected_pen);
	tickit_pen_unref(cfg->ro_unselected_pen);
	tickit_pen_unref(cfg->ro_urg_selected_pen);
	tickit_pen_unref(cfg->ro_urg_unselected_pen);

	destroy_sbar_cmds(cfg);
	tickit_pen_unref(cfg->statusbar_pen);

	destroy_tags(cfg);
	free(cfg->tag_printf);
	tickit_pen_unref(cfg->tag_selected_pen);
	tickit_pen_unref(cfg->tag_occupied_pen);
	tickit_pen_unref(cfg->tag_unoccupied_pen);
	tickit_pen_unref(cfg->tag_urgent_pen);

	destroy_bindings(&cfg->nstartups, &cfg->startups);
	destroy_bindings(&cfg->nkb_bindings, &cfg->kb_bindings);
	destroy_bindings(&cfg->nmterm_bindings, &cfg->mterm_bindings);
	destroy_bindings(&cfg->nmtbar_bindings, &cfg->mtbar_bindings);
	destroy_bindings(&cfg->nmsbar_bindings, &cfg->msbar_bindings);
	destroy_bindings(&cfg->nmtag_bindings, &cfg->mtag_bindings);
	destroy_bindings(&cfg->nmlayout_bindings, &cfg->mlayout_bindings);
	destroy_bindings(&cfg->nmframe_bindings, &cfg->mframe_bindings);

	destroy_layouts(cfg);
	destroy_colorschemes(cfg);
	destroy_colorrules(cfg);
}

static void preset_configs(Config *cfg) {
	cfg->selected_pen = tickit_pen_new();
	cfg->unselected_pen = tickit_pen_new();
	cfg->urg_selected_pen = tickit_pen_new();
	cfg->urg_unselected_pen = tickit_pen_new();
	cfg->ro_selected_pen = tickit_pen_new();
	cfg->ro_unselected_pen = tickit_pen_new();
	cfg->ro_urg_selected_pen = tickit_pen_new();
	cfg->ro_urg_unselected_pen = tickit_pen_new();

	cfg->statusbar_interval = 10;
	cfg->statusbar_pen = tickit_pen_new();
	cfg->statusbar_display = true;
	cfg->statusbar_top = true;
	cfg->statusbar_begin = ' ';
	cfg->statusbar_end = ' ';

	cfg->tag_selected_pen = tickit_pen_new();
	cfg->tag_occupied_pen = tickit_pen_new();
	cfg->tag_unoccupied_pen = tickit_pen_new();
	cfg->tag_urgent_pen = tickit_pen_new();

	cfg->zoomnum = 1;
	cfg->zoomsize = .5;
	cfg->scroll_history = 5000;
}

static void postset_configs(Config *cfg) {
	if (!cfg->colorschemes)
		error("You must define at least one colorscheme in configuration file \"%s\"", a4configfnameptr);

	currlayout = cfg->layouts;
	currzoomnum = cfg->zoomnum;
	currzoomsize = cfg->zoomsize;
}

static int parse_config(Config *cfg) {
	char fname[PATH_MAX];

	a4configfnameptr = a4configfname;

	preset_configs(cfg);

	if (!a4configfname) {
		getconfigfname(fname, "a4.ini");
		a4configfnameptr = fname;
	}

	int ret = ini_parse(a4configfnameptr, a4_ini_handler, cfg);
	if (ret < 0)
		error("Could not load configuration file \"%s\" (%d)", a4configfnameptr, ret);
	else if (ret)
		error("Error parsing line %d in configuration file \"%s\"", ret, a4configfnameptr);

	postset_configs(cfg);

	return 1;
}
