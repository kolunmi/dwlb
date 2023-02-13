#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <pixman-1/pixman.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utlist.h>
#include <wayland-client.h>

#include "utf8.h"
#include "xdg-shell-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

#define DIE(fmt, ...)						\
	do {							\
		fprintf(stderr, fmt "\n", ##__VA_ARGS__);	\
		exit(1);					\
	} while (0)

#define EDIE(fmt, ...)						\
	DIE(fmt ": %s", ##__VA_ARGS__, strerror(errno));

#define CLEANUP_DIE(why)			\
	do {					\
		cleanup();			\
		DIE(why);			\
	} while(0)

#define CLEANUP_EDIE(why)			\
	do {					\
		cleanup();			\
		EDIE(why);			\
	} while(0)

#define MIN(a, b)				\
	((a) < (b) ? (a) : (b))
#define MAX(a, b)				\
	((a) > (b) ? (a) : (b))

#define PROGRAM "dwlb"
#define VERSION "0.1"
#define USAGE								\
	"usage: dwlb [OPTIONS]\n"					\
	"Bar Config\n"							\
	"	-hide-vacant-tags		do not display empty and inactive tags\n" \
	"	-bottom				bars will initially be drawn at the bottom\n" \
	"	-hidden				bars will initially be hidden\n" \
	"	-font [FONT]			specify a font\n"	\
	"	-text-color [COLOR]		specify text color\n"	\
	"	-active-color [COLOR]		specify color to indicate active tags or monitors\n" \
	"	-inactive-color [COLOR]		specify color to indicate inactive tags or monitors\n" \
	"	-urg-text-color [COLOR]		specify text color on urgent tags\n" \
	"	-urg-bg-color [COLOR]		specify color of urgent tags\n"	\
	"	-tags [TAG 1]...[TAG 9]		specify custom tag names\n" \
	"Commands\n"							\
	"	-status	[OUTPUT] [TEXT]		set status text\n"	\
	"	-show [OUTPUT]			show bar\n"		\
	"	-hide [OUTPUT]			hide bar\n"		\
	"	-toggle-visibility [OUTPUT]	toggle bar visibility\n" \
	"	-set-top [OUTPUT]		draw bar at the top\n"	\
	"	-set-bottom [OUTPUT]		draw bar at the bottom\n" \
	"	-toggle-location [OUTPUT]	toggle bar location\n"	\
	"Other\n"							\
	"	-v				get version information\n" \
	"	-h				view this help text\n"

typedef struct Bar Bar;
struct Bar {
	struct zxdg_output_v1 *xdg_output;
	struct wl_output *wl_output;
	struct wl_surface *wl_surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	uint32_t registry_name;
	char *xdg_output_name;
	
	uint32_t width;
	uint32_t height;
	uint32_t textpadding;
	uint32_t stride;
	uint32_t bufsize;
	
	uint32_t mtags;
	uint32_t ctags;
	uint32_t urg;
	uint32_t selmon;
	char layout[32];
	char title[512];
	char status[512];

	bool hidden;
	bool bottom;
	bool redraw;

	Bar *prev, *next;
};

static int sock_fd;
static char socketdir[256];
static char *socketpath = NULL;
static char sockbuf[768];
static char *stdinbuf;
static size_t stdinbuf_cap;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zxdg_output_manager_v1 *output_manager;

static Bar *bars = NULL;

static uint32_t height;
static uint32_t textpadding;
static bool hidden = false;
static bool bottom = false;

static bool run_display = true;
static bool ready = false;
static bool hide_vacant = false;

#define TAGSLEN 9
static char *tags[TAGSLEN] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

static struct fcft_font *font;
static pixman_color_t activecolor = { .red = 0x0000, .green = 0x5555, .blue = 0x7777, .alpha = 0xffff, };
static pixman_color_t inactivecolor = { .red = 0x2222, .green = 0x2222, .blue = 0x2222, .alpha = 0xffff, };
static pixman_color_t textcolor = { .red = 0xeeee, .green = 0xeeee, .blue = 0xeeee, .alpha = 0xffff, };
static pixman_color_t urgbgcolor = { .red = 0xeeee, .green = 0xeeee, .blue = 0xeeee, .alpha = 0xffff, };
static pixman_color_t urgtextcolor = { .red = 2222, .green = 0x2222, .blue = 0x2222, .alpha = 0xffff, };


static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

/* Shared memory support function adapted from [wayland-book] */
static int
allocate_shm_file(size_t size)
{
	int fd = memfd_create("surface", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Color parsing logic adapted from [sway] */
static int
parse_color(const char *str, pixman_color_t *clr)
{
	if (*str == '#')
		str++;
	int len = strlen(str);

	// Disallows "0x" prefix that strtoul would ignore
	if ((len != 6 && len != 8) || !isxdigit(str[0]) || !isxdigit(str[1]))
		return -1;

	char *ptr;
	uint32_t parsed = strtoul(str, &ptr, 16);
	if (*ptr)
		return -1;

	if (len == 8) {
		clr->alpha = (parsed & 0xff) * 0x101;
		parsed >>= 8;
	} else {
		clr->alpha = 0xffff;
	}
	clr->red =   ((parsed >> 16) & 0xff) * 0x101;
	clr->green = ((parsed >>  8) & 0xff) * 0x101;
	clr->blue =  ((parsed >>  0) & 0xff) * 0x101;
	return 0;
}

static char *
handle_cmd(char *cmd, pixman_color_t *fg, pixman_color_t *bg,
	   pixman_color_t *def_fg, pixman_color_t *def_bg)
{
	char *arg, *end;

	if (!(arg = strchr(cmd, '(')) || !(end = strchr(arg + 1, ')')))
		return cmd;

	*arg++ = '\0';
	*end = '\0';

	if (!strcmp(cmd, "bg")) {
		if (bg && def_bg) {
			if (!*arg)
				*bg = *def_bg;
			else
				parse_color(arg, bg);
		}
	} else if (!strcmp(cmd, "fg")) {
		if (fg && def_fg) {
			if (!*arg)
				*fg = *def_fg;
			else
				parse_color(arg, fg);
		}
	}

	/* Restore string for later redraws */
	*--arg = '(';
	*end = ')';
	return end;
}

static uint32_t
draw_text(char *text,
	  uint32_t xpos,
	  uint32_t ypos,
	  pixman_image_t *foreground,
	  pixman_image_t *background,
	  pixman_color_t *fgcolor,
	  pixman_color_t *bgcolor,
	  uint32_t maxxpos,
	  uint32_t bufheight,
	  uint32_t padding,
	  bool commands)
{
	if (!*text || !maxxpos)
		return xpos;

	uint32_t ixpos = xpos;
	uint32_t nxpos;

	if ((nxpos = xpos + padding) + padding >= maxxpos)
		return xpos;
	xpos = nxpos;

	bool drawfg = foreground && fgcolor;
	bool drawbg = background && bgcolor;
	
	pixman_image_t *fgfill;
	pixman_color_t cur_fgcolor;
	pixman_color_t cur_bgcolor;
	if (drawfg) {
		fgfill = pixman_image_create_solid_fill(fgcolor);
		cur_fgcolor = *fgcolor;
	}
	if (drawbg)
		cur_bgcolor = *bgcolor;

	uint32_t codepoint;
	uint32_t state = UTF8_ACCEPT;
	uint32_t lastcp = 0;
	
	for (char *p = text; *p; p++) {
		/* If commands are enabled, check for inline ^ commands */
		if (commands && state == UTF8_ACCEPT && *p == '^') {
			p++;
			if (*p != '^') {
				p = handle_cmd(p, &cur_fgcolor, &cur_bgcolor, fgcolor, bgcolor);
				if (drawfg) {
					pixman_image_unref(fgfill);
					fgfill = pixman_image_create_solid_fill(&cur_fgcolor);
				}
				continue;
			}
		}

		/* Returns nonzero if more bytes are needed */
		if (utf8decode(&state, &codepoint, *p))
			continue;

		/* Turn off subpixel rendering, which complicates things when
		 * mixed with alpha channels */
		const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(font, codepoint, FCFT_SUBPIXEL_NONE);
		if (!glyph)
			continue;

		/* Adjust x position based on kerning with previous glyph */
		long x_kern = 0;
		if (lastcp)
			fcft_kerning(font, lastcp, codepoint, &x_kern, NULL);
		if ((nxpos = xpos + x_kern + glyph->advance.x) + padding > maxxpos)
			break;
		lastcp = codepoint;
		xpos += x_kern;

		if (drawfg) {
			/* Detect and handle pre-rendered glyphs (e.g. emoji) */
			if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
				/* Only the alpha channel of the mask is used, so we can
				 * use fgfill here to blend prerendered glyphs with the
				 * same opacity */
				pixman_image_composite32(
					PIXMAN_OP_OVER, glyph->pix, fgfill, foreground, 0, 0, 0, 0,
					xpos + glyph->x, ypos - glyph->y, glyph->width, glyph->height);
			} else {
				/* Applying the foreground color here would mess up
				 * component alphas for subpixel-rendered text, so we
				 * apply it when blending. */
				pixman_image_composite32(
					PIXMAN_OP_OVER, fgfill, glyph->pix, foreground, 0, 0, 0, 0,
					xpos + glyph->x, ypos - glyph->y, glyph->width, glyph->height);
			}
		}
		
		if (drawbg)
			pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
						&cur_bgcolor, 1, &(pixman_box32_t){
							.x1 = xpos, .x2 = nxpos,
							.y1 = 0, .y2 = bufheight
						});
		
		/* increment pen position */
		xpos = nxpos;
		ypos += glyph->advance.y;
	}
	if (drawfg)
		pixman_image_unref(fgfill);

	if (!lastcp)
		return ixpos;
	if (state != UTF8_ACCEPT)
		fprintf(stderr, "malformed UTF-8 sequence\n");
	
	nxpos = xpos + padding;
	if (drawbg) {
		/* Fill padding background */
		pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
					&cur_bgcolor, 1, &(pixman_box32_t){
						.x1 = ixpos, .x2 = ixpos + padding,
						.y1 = 0, .y2 = bufheight
					});
		pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
					bgcolor, 1, &(pixman_box32_t){
						.x1 = xpos, .x2 = nxpos,
						.y1 = 0, .y2 = bufheight
					});
	}
	
	return nxpos;
}

#define TEXT_WIDTH(text, maxwidth, padding, commands)	\
	draw_text(text, 0, 0, NULL, NULL, NULL, NULL, maxwidth, 0, padding, commands)

static int
draw_frame(Bar *b)
{
	/* Allocate buffer to be attached to the surface */
        int fd = allocate_shm_file(b->bufsize);
	if (fd == -1)
		return -1;

	uint32_t *data = mmap(NULL, b->bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return -1;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, b->bufsize);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, b->width, b->height, b->stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* Pixman image corresponding to main buffer */
	pixman_image_t *bar = pixman_image_create_bits(PIXMAN_a8r8g8b8, b->width, b->height, data, b->width * 4);
	
	/* Text background and foreground layers */
	pixman_image_t *foreground = pixman_image_create_bits(PIXMAN_a8r8g8b8, b->width, b->height, NULL, b->width * 4);
	pixman_image_t *background = pixman_image_create_bits(PIXMAN_a8r8g8b8, b->width, b->height, NULL, b->width * 4);
	
	/* Draw on images */
	uint32_t xpos_left = 0;
	uint32_t ypos = (b->height + font->ascent - font->descent) / 2;
	uint32_t boxs = font->height / 9;
	uint32_t boxw = font->height / 6 + 2;

	for (uint32_t i = 0; i < TAGSLEN; i++) {
		bool active = b->mtags & 1 << i;
		bool occupied = b->ctags & 1 << i;
		bool urgent = b->urg & 1 << i;

		if (hide_vacant && !active && !occupied && !urgent)
			continue;
		
		if (!hide_vacant && occupied)
			pixman_image_fill_boxes(PIXMAN_OP_SRC, foreground,
						&textcolor, 1, &(pixman_box32_t){
							.x1 = xpos_left + boxs, .x2 = xpos_left + boxs + boxw,
							.y1 = boxs, .y2 = boxs + boxw
						});
		
		if (urgent)
			xpos_left = draw_text(tags[i], xpos_left, ypos, foreground, background, &urgtextcolor,
					      &urgbgcolor, b->width, b->height, b->textpadding, false);
		else
			xpos_left = draw_text(tags[i], xpos_left, ypos, foreground, background, &textcolor,
					      active ? &activecolor : &inactivecolor, b->width, b->height,
					      b->textpadding, false);
	}
	
	xpos_left = draw_text(b->layout, xpos_left, ypos, foreground, background, &textcolor,
			      &inactivecolor, b->width, b->height, b->textpadding, false);

	uint32_t status_width = TEXT_WIDTH(b->status, b->width - xpos_left, b->textpadding, true);
	draw_text(b->status, b->width - status_width, ypos, foreground, background, &textcolor,
		  &inactivecolor, b->width, b->height, b->textpadding, true);

	xpos_left = draw_text(b->title, xpos_left, ypos, foreground, background, &textcolor,
			      b->selmon ? &activecolor : &inactivecolor, b->width - status_width,
			      b->height, b->textpadding, false);

	pixman_image_fill_boxes(PIXMAN_OP_SRC, background,
				b->selmon ? &activecolor : &inactivecolor, 1,
				&(pixman_box32_t){
					.x1 = xpos_left, .x2 = b->width - status_width,
					.y1 = 0, .y2 = b->height
				});

	/* Draw background and foreground on bar */
	pixman_image_composite32(PIXMAN_OP_OVER, background, NULL, bar, 0, 0, 0, 0, 0, 0, b->width, b->height);
	pixman_image_composite32(PIXMAN_OP_OVER, foreground, NULL, bar, 0, 0, 0, 0, 0, 0, b->width, b->height);

	pixman_image_unref(foreground);
	pixman_image_unref(background);
	pixman_image_unref(bar);
	
	munmap(data, b->bufsize);

	wl_surface_attach(b->wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(b->wl_surface, 0, 0, b->width, b->height);
	wl_surface_commit(b->wl_surface);

	return 0;
}

/* Layer-surface setup adapted from layer-shell example in [wlroots] */
static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
			uint32_t serial, uint32_t w, uint32_t h)
{
	Bar *b = (Bar *)data;
	
	b->width = w;
	b->height = h;
	b->stride = b->width * 4;
	b->bufsize = b->stride * b->height;

	zwlr_layer_surface_v1_set_exclusive_zone(b->layer_surface, b->height);
	zwlr_layer_surface_v1_ack_configure(surface, serial);

	draw_frame(b);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	Bar *b = (Bar *)data;
	
	zwlr_layer_surface_v1_destroy(surface);
	wl_surface_destroy(b->wl_surface);
	run_display = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void
cleanup(void)
{
	if (socketpath)
		unlink(socketpath);
}

static void
output_name(void *data, struct zxdg_output_v1 *xdg_output, const char *name)
{
	Bar *b = (Bar *)data;
	
	/* Is this necessary? */
	if (b->xdg_output_name)
		free(b->xdg_output_name);
	if (!(b->xdg_output_name = strdup(name)))
		CLEANUP_EDIE("strdup");
}

static void
output_logical_position(void *data, struct zxdg_output_v1 *xdg_output,
			int32_t x, int32_t y)
{
}

static void
output_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
		    int32_t width, int32_t height)
{
}

static void
output_done(void *data, struct zxdg_output_v1 *xdg_output)
{
}

static void
output_description(void *data, struct zxdg_output_v1 *xdg_output,
		   const char *description)
{
}

static const struct zxdg_output_v1_listener output_listener = {
	.name = output_name,
	.logical_position = output_logical_position,
	.logical_size = output_logical_size,
	.done = output_done,
	.description = output_description
};

static void
show_bar(Bar *b)
{
	b->wl_surface = wl_compositor_create_surface(compositor);
	if (!b->wl_surface)
		CLEANUP_DIE("Could not create wl_surface");

	b->layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, b->wl_surface, b->wl_output,
								 ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, PROGRAM);
	if (!b->layer_surface)
		CLEANUP_DIE("Could not create layer_surface");
	zwlr_layer_surface_v1_add_listener(b->layer_surface, &layer_surface_listener, b);

	zwlr_layer_surface_v1_set_size(b->layer_surface, 0, b->height);
	zwlr_layer_surface_v1_set_anchor(b->layer_surface,
					 (b->bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	wl_surface_commit(b->wl_surface);
	
	b->hidden = false;
}

static void
hide_bar(Bar *b)
{
	zwlr_layer_surface_v1_destroy(b->layer_surface);
	wl_surface_destroy(b->wl_surface);
	b->hidden = true;
}

static void
setup_bar(Bar *b)
{
	b->height = height;
	b->textpadding = textpadding;
	b->bottom = bottom;
	b->hidden = hidden;

	snprintf(b->layout, sizeof b->layout, "[]=");
		
	b->xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, b->wl_output);
	if (!b->xdg_output)
		CLEANUP_DIE("Could not create xdg_output");
	zxdg_output_v1_add_listener(b->xdg_output, &output_listener, b);

	if (!b->hidden)
		show_bar(b);
}

static void
handle_global(void *data, struct wl_registry *registry,
	      uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		Bar *b = malloc(sizeof(Bar));
		if (!b)
			CLEANUP_EDIE("malloc");
		memset(b, 0, sizeof(Bar));
		b->registry_name = name;
		b->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
		DL_APPEND(bars, b);
		if (ready)
			setup_bar(b);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		output_manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2);
	}
}

static void
teardown_bar(Bar *b)
{
	zxdg_output_v1_destroy(b->xdg_output);
	wl_surface_destroy(b->wl_surface);
	zwlr_layer_surface_v1_destroy(b->layer_surface);
	if (b->xdg_output_name)
		free(b->xdg_output_name);
	free(b);
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	Bar *b;
	DL_FOREACH(bars, b)
		if (b->registry_name == name)
			break;
	
	if (!b)
		return;
	
	DL_DELETE(bars, b);
	teardown_bar(b);
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove
};

static int
advance_word(char **beg, char **end)
{
	for (*beg = *end; **beg == ' '; (*beg)++);
	for (*end = *beg; **end && **end != ' '; (*end)++);
	if (!**end)
		/* last word */
		return -1;
	**end = '\0';
	(*end)++;
	return 0;
}

#define ADVANCE() advance_word(&wordbeg, &wordend)
#define ADVANCE_IF_LAST_CONT() if (ADVANCE() == -1) continue
#define ADVANCE_IF_LAST_BREAK() if (ADVANCE() == -1) break

static void
read_stdin(void)
{
	size_t len = 0;
	for (;;) {
		ssize_t rv = read(STDIN_FILENO, stdinbuf + len, stdinbuf_cap - len);
		if (rv == -1) {
			if (errno == EWOULDBLOCK)
				break;
			CLEANUP_EDIE("read");
		}
		if (rv == 0) {
			run_display = 0;
			return;
		}

		if ((len += rv) > stdinbuf_cap / 2)
			if (!(stdinbuf = realloc(stdinbuf, (stdinbuf_cap *= 2))))
				CLEANUP_EDIE("realloc");
	}

	char *linebeg, *lineend;
	char *wordbeg, *wordend;
	
	for (linebeg = stdinbuf;
	     (lineend = memchr(linebeg, '\n', stdinbuf + len - linebeg));
	     linebeg = lineend) {
		*lineend++ = '\0';
		wordend = linebeg;

		ADVANCE_IF_LAST_CONT();

		Bar *b;
		DL_FOREACH(bars, b)
			if (b->xdg_output_name)
				if (!strcmp(wordbeg, b->xdg_output_name))
					break;
		if (!b)
			continue;
		
		ADVANCE_IF_LAST_CONT();

		uint32_t val;
		if (!strcmp(wordbeg, "tags")) {
			ADVANCE_IF_LAST_CONT();
			if ((val = atoi(wordbeg)) != b->ctags) {
				b->ctags = val;
				b->redraw = true;
			}
			ADVANCE_IF_LAST_CONT();
			if ((val = atoi(wordbeg)) != b->mtags) {
				b->mtags = val;
				b->redraw = true;
			}
			ADVANCE_IF_LAST_CONT();
			/* skip sel */
			ADVANCE();
			if ((val = atoi(wordbeg)) != b->urg) {
				b->urg = val;
				b->redraw = true;
			}
		} else if (!strcmp(wordbeg, "layout")) {
			if (strcmp(b->layout, wordend) != 0) {
				snprintf(b->layout, sizeof b->layout, "%s", wordend);
				b->redraw = true;
			}
		} else if (!strcmp(wordbeg, "title")) {
			if (strcmp(b->title, wordend) != 0) {
				snprintf(b->title, sizeof b->title, "%s", wordend);
				b->redraw = true;
			}
		} else if (!strcmp(wordbeg, "selmon")) {
			ADVANCE();
			if ((val = atoi(wordbeg)) != b->selmon) {
				b->selmon = val;
				b->redraw = true;
			}
		}
	}
}

static void
bar_set_top(Bar *b)
{
	if (!b->hidden) {
		zwlr_layer_surface_v1_set_anchor(b->layer_surface,
						 ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		b->redraw = true;
	}
	b->bottom = false;
}

static void
bar_set_bottom(Bar *b)
{
	if (!b->hidden) {
		zwlr_layer_surface_v1_set_anchor(b->layer_surface,
						 ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		b->redraw = true;
	}
	b->bottom = true;
}

static void
read_socket(void)
{
	int cli_fd;
	if ((cli_fd = accept(sock_fd, NULL, 0)) == -1)
		CLEANUP_EDIE("accept");
	ssize_t len = recv(cli_fd, sockbuf, sizeof sockbuf - 1, 0);
	if (len == -1)
		CLEANUP_EDIE("recv");
	close(cli_fd);
	if (len == 0)
		return;
	sockbuf[len] = '\0';

	do {
		char *wordbeg, *wordend;
		wordend = (char *)&sockbuf;

		ADVANCE_IF_LAST_BREAK();
		
		Bar *b;
		bool all = false;
		
		if (!strcmp(wordbeg, "all")) {
			all = true;
		} else if (!strcmp(wordbeg, "selected")) {
			DL_FOREACH(bars, b)
				if (b->selmon)
					break;
		} else {
			DL_FOREACH(bars, b)
				if (b->xdg_output_name)
					if (!strcmp(wordbeg, b->xdg_output_name))
						break;
		}
		
		if (!all && !b)
			break;

		ADVANCE();

		if (!strcmp(wordbeg, "status")) {
			if (!wordend)
				break;
			if (all) {
				DL_FOREACH(bars, b) {
					if (strcmp(b->status, wordend) != 0) {
						snprintf(b->status, sizeof b->status, "%s", wordend);
						b->redraw = true;
					}
				}
			} else {
				if (strcmp(b->status, wordend) != 0) {
					snprintf(b->status, sizeof b->status, "%s", wordend);
					b->redraw = true;
				}
			}
		} else if (!strcmp(wordbeg, "show")) {
			if (all) {
				DL_FOREACH(bars, b)
					if (b->hidden)
						show_bar(b);
			} else {
				if (b->hidden)
					show_bar(b);
			}
		} else if (!strcmp(wordbeg, "hide")) {
			if (all) {
				DL_FOREACH(bars, b)
					if (!b->hidden)
						hide_bar(b);
			} else {
				if (!b->hidden)
					hide_bar(b);
			}
		} else if (!strcmp(wordbeg, "toggle-visibility")) {
			if (all) {
				DL_FOREACH(bars, b)
					if (b->hidden)
						show_bar(b);
					else
						hide_bar(b);
			} else {
				if (b->hidden)
					show_bar(b);
				else
					hide_bar(b);
			}
		} else if (!strcmp(wordbeg, "set-top")) {
			if (all) {
				DL_FOREACH(bars, b)
					if (b->bottom)
						bar_set_top(b);
						
			} else {
				if (b->bottom)
					bar_set_top(b);
			}
		} else if (!strcmp(wordbeg, "set-bottom")) {
			if (all) {
				DL_FOREACH(bars, b)
					if (!b->bottom)
						bar_set_bottom(b);
						
			} else {
				if (!b->bottom)
					bar_set_bottom(b);
			}
		} else if (!strcmp(wordbeg, "toggle-location")) {
			if (all) {
				DL_FOREACH(bars, b)
					if (b->bottom)
						bar_set_top(b);
					else
						bar_set_bottom(b);
			} else {
				if (b->bottom)
					bar_set_top(b);
				else
					bar_set_bottom(b);
			}
		}
	} while (0);
}

static void
event_loop(void)
{
	int wl_fd = wl_display_get_fd(display);

	while (run_display) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(STDIN_FILENO, &rfds);
		FD_SET(sock_fd, &rfds);
		FD_SET(wl_fd, &rfds);

		/* Does this need to be inside the loop? */
		wl_display_flush(display);

		if (select(MAX(sock_fd, wl_fd) + 1, &rfds, NULL, NULL, NULL) == -1)
			continue;

		if (FD_ISSET(STDIN_FILENO, &rfds))
			read_stdin();

		if (FD_ISSET(sock_fd, &rfds))
			read_socket();
		
		if (FD_ISSET(wl_fd, &rfds))
			if (wl_display_dispatch(display) == -1)
				break;

		Bar *b;
		DL_FOREACH(bars, b) {
			if (b->redraw) {
				if (!b->hidden)
					draw_frame(b);
				b->redraw = false;
			}
		}
	}
}

static void
client_send_command(struct sockaddr_un *sock_address, const char *output,
		    const char *cmd, const char *data)
{
	DIR *dir;
	if (!(dir = opendir(socketdir)))
		EDIE("Could not open directory '%s'", socketdir);

	if (data)
		snprintf(sockbuf, sizeof sockbuf, "%s %s %s", output, cmd, data);
	else
		snprintf(sockbuf, sizeof sockbuf, "%s %s", output, cmd);
	
	size_t len = strlen(sockbuf);
			
	struct dirent *de;
	bool newfd = true;
	while ((de = readdir(dir))) {
		if (!strncmp(de->d_name, "dwlb-", 5)) {
			if (newfd)
				if ((sock_fd = socket(AF_UNIX, SOCK_STREAM, 1)) == -1)
					EDIE("socket");
			snprintf(sock_address->sun_path, sizeof sock_address->sun_path, "%s/%s", socketdir, de->d_name);
			if (connect(sock_fd, (struct sockaddr *) sock_address, sizeof(*sock_address)) == -1) {
				newfd = false;
				continue;
			}
					
			if (send(sock_fd, sockbuf, len, 0) == -1)
				fprintf(stderr, "Could not send status data to '%s'\n", sock_address->sun_path);
			close(sock_fd);
			newfd = true;
		}
	}
			
	closedir(dir);
}

void
sig_handler(int sig)
{
	if (sig == SIGINT || sig == SIGHUP || sig == SIGTERM)
		run_display = false;
}

int
main(int argc, char **argv)
{
	char *fontstr = "";
	char *xdgruntimedir;
	struct sockaddr_un sock_address;
	Bar *b, *t;

	/* Establish socket directory */
	if (!(xdgruntimedir = getenv("XDG_RUNTIME_DIR")))
		DIE("Could not retrieve XDG_RUNTIME_DIR");
	snprintf(socketdir, sizeof socketdir, "%s/dwlb", xdgruntimedir);
	if (mkdir(socketdir, S_IRWXU) == -1)
		if (errno != EEXIST)
			EDIE("Could not create directory '%s'", socketdir);
	sock_address.sun_family = AF_UNIX;

	/* Parse options */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-status")) {
			if (++i + 1 >= argc)
				DIE("Option -status requires two arguments");
			client_send_command(&sock_address, argv[i], "status", argv[i + 1]);
			return 0;
		} else if (!strcmp(argv[i], "-show")) {
			if (++i >= argc)
				DIE("Option -show requires an argument");
			client_send_command(&sock_address, argv[i], "show", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-hide")) {
			if (++i >= argc)
				DIE("Option -hide requires an argument");
			client_send_command(&sock_address, argv[i], "hide", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-toggle-visibility")) {
			if (++i >= argc)
				DIE("Option -toggle requires an argument");
			client_send_command(&sock_address, argv[i], "toggle-visibility", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-set-top")) {
			if (++i >= argc)
				DIE("Option -set-top requires an argument");
			client_send_command(&sock_address, argv[i], "set-top", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-set-bottom")) {
			if (++i >= argc)
				DIE("Option -set-bottom requires an argument");
			client_send_command(&sock_address, argv[i], "set-bottom", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-toggle-location")) {
			if (++i >= argc)
				DIE("Option -toggle-location requires an argument");
			client_send_command(&sock_address, argv[i], "toggle-location", NULL);
			return 0;
		} else if (!strcmp(argv[i], "-hide-vacant-tags")) {
			hide_vacant = true;
		} else if (!strcmp(argv[i], "-bottom")) {
			bottom = true;
		} else if (!strcmp(argv[i], "-hidden")) {
			hidden = true;
		} else if (!strcmp(argv[i], "-font")) {
			if (++i >= argc)
				DIE("Option -font requires an argument");
			fontstr = argv[i];
		} else if (!strcmp(argv[i], "-text-color")) {
			if (++i >= argc)
				DIE("Option -text-color requires an argument");
			if (parse_color(argv[i], &textcolor) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-active-color")) {
			if (++i >= argc)
				DIE("Option -active-color requires an argument");
			if (parse_color(argv[i], &activecolor) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-inactive-color")) {
			if (++i >= argc)
				DIE("Option -inactive-color requires an argument");
			if (parse_color(argv[i], &inactivecolor) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-urg-text-color")) {
			if (++i >= argc)
				DIE("Option -urg-text-color requires an argument");
			if (parse_color(argv[i], &urgtextcolor) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-urg-bg-color")) {
			if (++i >= argc)
				DIE("Option -urg-bg-color requires an argument");
			if (parse_color(argv[i], &urgbgcolor) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-tags")) {
			if (i + TAGSLEN >= argc)
				DIE("Option -tags requires %i arguments", TAGSLEN);
			for (int j = 0; j < TAGSLEN; j++)
				tags[j] = argv[i + 1 + j];
			i += TAGSLEN;
		} else if (!strcmp(argv[i], "-v")) {
			fprintf(stderr, PROGRAM " " VERSION "\n");
			return 0;
		} else if (!strcmp(argv[i], "-h")) {
			fprintf(stderr, USAGE);
			return 0;
		} else {
			DIE("Option '%s' not recognized\n" USAGE, argv[i]);
		}
	}

	/* Set up display and protocols */
	display = wl_display_connect(NULL);
	if (!display)
		DIE("Failed to create display");

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell || !output_manager)
		DIE("Compositor does not support all needed protocols");

	/* Load selected font */
	fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_ERROR);
	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);
	font = fcft_from_name(1, (const char *[]) {fontstr}, NULL);
	if (!font)
		DIE("Could not load font");
	textpadding = font->height / 2;
	height = font->ascent + font->descent;
	
	/* Setup bars */
	DL_FOREACH(bars, b)
		setup_bar(b);
	wl_display_roundtrip(display);

	/* Configure stdin */
	if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1)
		EDIE("fcntl");
	
	/* Allocate stdin buffer */
	if (!(stdinbuf = malloc(1024)))
		EDIE("malloc");
	stdinbuf_cap = 1024;
	
	/* Set up socket */
	for (uint32_t i = 0;; i++) {
		if ((sock_fd = socket(AF_UNIX, SOCK_STREAM, 1)) == -1)
			DIE("socket");
		snprintf(sock_address.sun_path, sizeof sock_address.sun_path, "%s/dwlb-%i", socketdir, i);
		if (connect(sock_fd, (struct sockaddr *) &sock_address, sizeof sock_address) == -1)
			break;
		close(sock_fd);
	}

	socketpath = (char *)&sock_address.sun_path;
	unlink(socketpath);
	if (bind(sock_fd, (struct sockaddr *) &sock_address, sizeof sock_address) == -1)
		CLEANUP_EDIE("bind");
	if (listen(sock_fd, SOMAXCONN) == -1)
		CLEANUP_EDIE("listen");
	fcntl(sock_fd, F_SETFD, FD_CLOEXEC | fcntl(sock_fd, F_GETFD));

	/* Set up signals */
	signal(SIGINT, sig_handler);
	signal(SIGHUP, sig_handler);
	signal(SIGTERM, sig_handler);

	/* Run */
	ready = true;
	event_loop();

	/* Clean everything up */
	close(sock_fd);
	unlink(socketpath);
	
	zwlr_layer_shell_v1_destroy(layer_shell);
	zxdg_output_manager_v1_destroy(output_manager);
	
	DL_FOREACH_SAFE(bars, b, t)
		teardown_bar(b);
	
	fcft_destroy(font);
	fcft_fini();
	
	wl_shm_destroy(shm);
	wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);

	return 0;
}
