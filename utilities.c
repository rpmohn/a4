#include <strings.h>

/* macros*/
#define MAX(x, y)   ((x) > (y) ? (x) : (y))
#define MIN(x, y)   ((x) < (y) ? (x) : (y))

/* typedefs */
/*
typedef struct NameValue NameValue;
struct NameValue {
	char *name;
	char *value;
	NameValue *next;
};
*/

/* function declarations */
static void error(const char *errstr, ...);
static bool checkshell(const char *shell, const char *application_name);
static const char *getshell(const char *application_name);

/*
static void printVTermColor(VTermColor *c);
static void printVTermCell_Colors(VTermScreenCell *cell);
static void printVTermCell_Info(VTermScreenCell *cell);
static void printTickitPen(TickitPen *pen);
*/

/*
static NameValue *get_NameValue(NameValue *set, const char *key);
static void destroy_NameValues(NameValue *set);
static void load_NameValues(NameValue **set, const char *fname);
*/

/* functions */
static void error(const char *errstr, ...) {
	fprintf(stderr, "ERROR: ");
	va_list ap;
	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

static bool checkshell(const char *shell, const char *application_name) {
	if (shell == NULL || *shell == '\0' || *shell != '/')
		return false;
	if (!strcmp(strrchr(shell, '/')+1, application_name))
		return false;
	if (access(shell, X_OK))
		return false;
	return true;
}

static const char *getshell(const char *application_name) {
	const char *shell = getenv("SHELL");
	struct passwd *pw;

	if (checkshell(shell, application_name))
		return shell;
	if ((pw = getpwuid(getuid())) && checkshell(pw->pw_shell, application_name))
		return pw->pw_shell;
	return "/bin/sh";
}

/*
static NameValue *get_NameValue(NameValue *set, const char *key) {
	NameValue *item = set;
	while (item) {
		if (strcasecmp(key, item->name) == 0)
			return item;
		item = item->next;
	}
	return NULL;
}

static void destroy_NameValues(NameValue *set) {
	NameValue *next;
	while (set) {
		next = set->next;
		free(set->name);
		free(set->value);
		free(set);
		set = next;
	}
}

static void load_NameValues(NameValue **set, const char *fname) {
	unsigned int i;
	char *str, *tok, *save;
	char line[MAX_STR];

	FILE *fp = fopen(fname, "r");
	if (!fp)
		error("Could not open file %s", fname);
	while (fgets(line, MAX_STR, fp)) {
		NameValue *item = malloc(sizeof(NameValue));
		item->next = *set;
		*set = item;

		save = NULL;
		str = strdup(line);
		for (i = 0, tok = strtok_r(str, " 	", &save);
				tok != NULL;
				i++, tok = strtok_r(NULL, " 	", &save)) {
			if (i == 0) {
				item->name = strdup(tok);
			} else if (i == 1) {
				item->value = strdup(tok);
			} else {
				error("Invalid Name-Value line in file %s: %s", fname, line);
			}
		}
		free(str);
	}
	fclose(fp);
}
*/

/*
static void printVTermColor(VTermColor *c) {
	if (VTERM_COLOR_IS_RGB(c))
		tickit_debug_logf("UVC", "|  |  color is 0x%02X%02X%02X", c->rgb.red, c->rgb.green, c->rgb.blue);
	if (VTERM_COLOR_IS_INDEXED(c))
		tickit_debug_logf("UVC", "|  |  color is %d", c->indexed.idx);
	if (VTERM_COLOR_IS_DEFAULT_FG(c))
		tickit_debug_logf("UVC", "|  |  |  this is also the default fg");
	if (VTERM_COLOR_IS_DEFAULT_BG(c))
		tickit_debug_logf("UVC", "|  |  |  this is also the default bg");
}

static void printVTermCell_Colors(VTermScreenCell *cell) {
	tickit_debug_logf("UCC", "|  VTermCell fg color:");
	printVTermColor(&cell->fg);
	tickit_debug_logf("UCC", "|  VTermCell bg color:");
	printVTermColor(&cell->bg);
}

static void printVTermCell_Info(VTermScreenCell *cell) {
	tickit_debug_logf("UCI", "VTermCell char: %c(%02X)", cell->chars[0], cell->chars[0]);
	printVTermCell_Colors(cell);
}

static void printTickitPen(TickitPen *pen) {
	int idx;
	TickitPenRGB8 rgb;

	idx = tickit_pen_get_colour_attr(pen, TICKIT_PEN_FG);
	if (tickit_pen_has_colour_attr_rgb8(pen, TICKIT_PEN_FG)) {
		rgb = tickit_pen_get_colour_attr_rgb8(pen, TICKIT_PEN_FG);
		tickit_debug_logf("UTP", "pen.fg = %03d/0x%02X%02X%02X", idx, rgb.r, rgb.g, rgb.b);
	} else {
		tickit_debug_logf("UTP", "pen.fg = %03d/------", idx);
	}

	idx = tickit_pen_get_colour_attr(pen, TICKIT_PEN_BG);
	if (tickit_pen_has_colour_attr_rgb8(pen, TICKIT_PEN_BG)) {
		rgb = tickit_pen_get_colour_attr_rgb8(pen, TICKIT_PEN_BG);
		tickit_debug_logf("UTP", "pen.bg = %03d/0x%02X%02X%02X", idx, rgb.r, rgb.g, rgb.b);
	} else {
		tickit_debug_logf("UTP", "pen.bg = %03d/------", idx);
	}
}
*/
