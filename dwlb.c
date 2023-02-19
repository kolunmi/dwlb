#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pixman-1/pixman.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utlist.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

#include "utf8.h"
#include "xdg-shell-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

#define DIE(fmt, ...)				\
	do {							\
		cleanup();					\
		fprintf(stderr, fmt "\n", ##__VA_ARGS__);	\
		exit(1);					\
	} while (0)
#define EDIE(fmt, ...)						\
	DIE(fmt ": %s", ##__VA_ARGS__, strerror(errno));

#define MIN(a, b)				\
	((a) < (b) ? (a) : (b))
#define MAX(a, b)				\
	((a) > (b) ? (a) : (b))
#define LENGTH(x)				\
	(sizeof x / sizeof x[0])

#define ARRAY_INIT_CAP 8
#define ARRAY_EXPAND(arr, len, cap, inc)				\
	do {								\
		uint32_t new_len, new_cap;				\
		new_len = (len) + (inc);				\
		if (new_len > (cap)) {					\
			new_cap = new_len * 2;				\
			if (new_cap < ARRAY_INIT_CAP)			\
				new_cap = ARRAY_INIT_CAP;		\
			if (!((arr) = realloc((arr), sizeof(*(arr)) * new_cap))) \
				EDIE("realloc");		\
			(cap) = new_cap;				\
		}							\
		(len) = new_len;					\
	} while (0)
#define ARRAY_APPEND(arr, len, cap, ptr)		\
	do {						\
		ARRAY_EXPAND((arr), (len), (cap), 1);	\
		(ptr) = &(arr)[(len) - 1];		\
	} while (0)

#define PROGRAM "dwlb"
#define VERSION "0.1"
#define USAGE								\
	"usage: dwlb [OPTIONS]\n"					\
	"Bar Config\n"							\
	"	-hidden				bars will initially be hidden\n" \
	"	-no-hidden			bars will not initially be hidden\n" \
	"	-bottom				bars will initially be drawn at the bottom\n" \
	"	-no-bottom			bars will initially be drawn at the top\n" \
	"	-hide-vacant-tags		do not display empty and inactive tags\n" \
	"	-no-hide-vacant-tags		display empty and inactive tags\n" \
	"	-status-commands		enable in-line commands in status text\n" \
	"	-no-status-commands		disable in-line commands in status text\n" \
	"	-font [FONT]			specify a font\n"	\
	"	-tags [FIRST TAG]...[LAST TAG]	specify custom tag names\n" \
	"	-vertical-padding [PIXELS]	specify vertical pixel padding above and below text\n" \
	"	-active-fg-color [COLOR]	specify text color of active tags or monitors\n" \
	"	-active-bg-color [COLOR]	specify background color of active tags or monitors\n" \
	"	-inactive-fg-color [COLOR]	specify text color of inactive tags or monitors\n" \
	"	-inactive-fg-color [COLOR]	specify background color of inactive tags or monitors\n" \
	"	-urgent-fg-color [COLOR]	specify text color of urgent tags\n" \
	"	-urgent-bg-color [COLOR]	specify background color of urgent tags\n" \
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

typedef struct {
	pixman_color_t color;
	bool bg;
	char *start;
} StatusColor;

typedef struct {
	uint32_t button;
	uint32_t x1;
	uint32_t x2;
	bool incomplete;
	char command[128];
	char *start;
	char *end;
} StatusButton;

typedef struct Bar Bar;
struct Bar {
	struct zxdg_output_v1 *xdg_output;
	struct wl_output *wl_output;
	struct wl_surface *wl_surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	uint32_t registry_name;
	char *xdg_output_name;

	bool configured;
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
	
	StatusColor *status_colors;
	uint32_t status_colors_l, status_colors_c;
	StatusButton *status_buttons;
	uint32_t status_buttons_l, status_buttons_c;

	bool hidden;
	bool bottom;
	
	bool redraw;

	Bar *prev, *next;
};

typedef struct Seat Seat;
struct Seat {
	struct wl_seat *wl_seat;
	struct wl_pointer *wl_pointer;

	uint32_t registry_name;

	Bar *pointer_bar;
	uint32_t pointer_x;
	uint32_t pointer_y;
	uint32_t pointer_button;

	Seat *prev, *next;
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
static struct wl_cursor_image *cursor_image;
static struct wl_surface *cursor_surface;

static Bar *bar_list = NULL;
static Seat *seat_list = NULL;

static struct fcft_font *font;

static uint32_t height;
static uint32_t textpadding;

static bool run_display = true;
static bool ready = false;

#include "config.h"

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

static uint32_t
draw_text(char *text,
	  uint32_t xpos,
	  uint32_t ypos,
	  pixman_image_t *foreground,
	  pixman_image_t *background,
	  pixman_color_t *fg_color,
	  pixman_color_t *bg_color,
	  uint32_t max_xpos,
	  uint32_t buf_height,
	  uint32_t padding,
	  StatusColor *colors,
	  uint32_t colors_l,
	  StatusButton *buttons,
	  uint32_t buttons_l)
{
	if (!*text || !max_xpos)
		return xpos;

	uint32_t ixpos = xpos;
	uint32_t nxpos;

	if ((nxpos = xpos + padding) + padding >= max_xpos)
		return xpos;
	xpos = nxpos;

	bool draw_fg = foreground && fg_color;
	bool draw_bg = background && bg_color;
	
	pixman_image_t *fg_fill;
	pixman_color_t *cur_bg_color;
	if (draw_fg)
		fg_fill = pixman_image_create_solid_fill(fg_color);
	if (draw_bg)
		cur_bg_color = bg_color;

	if (buttons)
		for (uint32_t i = 0; i < buttons_l; i++)
			buttons[i].incomplete = true;
	
	uint32_t codepoint;
	uint32_t state = UTF8_ACCEPT;
	uint32_t last_cp = 0;
	uint32_t color_ind = 0;

	for (char *p = text; *p; p++) {
		/* Check for color changes */
		if (state == UTF8_ACCEPT) {
			if (colors && (draw_fg || draw_bg)) {
				while (color_ind < colors_l && p == colors[color_ind].start) {
					if (colors[color_ind].bg) {
						if (draw_bg)
							cur_bg_color = &colors[color_ind].color;
					} else {
						if (draw_fg) {
							pixman_image_unref(fg_fill);
							fg_fill = pixman_image_create_solid_fill(&colors[color_ind].color);
						}
					}
					color_ind++;
				}
			}
			
			if (buttons) {
				for (uint32_t i = 0; i < buttons_l; i++) {
					if (p == buttons[i].start) {
						buttons[i].x1 = xpos;
					} else if (p == buttons[i].end) {
						buttons[i].x2 = xpos;
						buttons[i].incomplete = false;
					}
				}
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
		if (last_cp)
			fcft_kerning(font, last_cp, codepoint, &x_kern, NULL);
		if ((nxpos = xpos + x_kern + glyph->advance.x) + padding > max_xpos)
			break;
		last_cp = codepoint;
		xpos += x_kern;

		if (draw_fg) {
			/* Detect and handle pre-rendered glyphs (e.g. emoji) */
			if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
				/* Only the alpha channel of the mask is used, so we can
				 * use fgfill here to blend prerendered glyphs with the
				 * same opacity */
				pixman_image_composite32(
					PIXMAN_OP_OVER, glyph->pix, fg_fill, foreground, 0, 0, 0, 0,
					xpos + glyph->x, ypos - glyph->y, glyph->width, glyph->height);
			} else {
				/* Applying the foreground color here would mess up
				 * component alphas for subpixel-rendered text, so we
				 * apply it when blending. */
				pixman_image_composite32(
					PIXMAN_OP_OVER, fg_fill, glyph->pix, foreground, 0, 0, 0, 0,
					xpos + glyph->x, ypos - glyph->y, glyph->width, glyph->height);
			}
		}
		
		if (draw_bg)
			pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
						cur_bg_color, 1, &(pixman_box32_t){
							.x1 = xpos, .x2 = nxpos,
							.y1 = 0, .y2 = buf_height
						});
		
		/* increment pen position */
		xpos = nxpos;
		ypos += glyph->advance.y;
	}
	
	if (draw_fg)
		pixman_image_unref(fg_fill);
	if (!last_cp)
		return ixpos;
	if (buttons)
		for (uint32_t i = 0; i < buttons_l; i++)
			if (buttons[i].incomplete)
				buttons[i].x2 = xpos;
	
	nxpos = xpos + padding;
	if (draw_bg) {
		/* Fill padding background */
		pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
					bg_color, 1, &(pixman_box32_t){
						.x1 = ixpos, .x2 = ixpos + padding,
						.y1 = 0, .y2 = buf_height
					});
		pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
					bg_color, 1, &(pixman_box32_t){
						.x1 = xpos, .x2 = nxpos,
						.y1 = 0, .y2 = buf_height
					});
	}
	
	return nxpos;
}

#define TEXT_WIDTH(text, maxwidth, padding)				\
	draw_text(text, 0, 0, NULL, NULL, NULL, NULL, maxwidth, 0, padding, NULL, 0, NULL, 0)

static int
draw_frame(Bar *bar)
{
	/* Allocate buffer to be attached to the surface */
        int fd = allocate_shm_file(bar->bufsize);
	if (fd == -1)
		return -1;

	uint32_t *data = mmap(NULL, bar->bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return -1;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bar->bufsize);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, bar->width, bar->height, bar->stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* Pixman image corresponding to main buffer */
	pixman_image_t *final = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, data, bar->width * 4);
	
	/* Text background and foreground layers */
	pixman_image_t *foreground = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, NULL, bar->width * 4);
	pixman_image_t *background = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, NULL, bar->width * 4);
	
	/* Draw on images */
	uint32_t xpos_left = 0;
	uint32_t ypos = (bar->height + font->ascent - font->descent) / 2;
	uint32_t boxs = font->height / 9;
	uint32_t boxw = font->height / 6 + 2;

	for (uint32_t i = 0; i < LENGTH(tags); i++) {
		bool active = bar->mtags & 1 << i;
		bool occupied = bar->ctags & 1 << i;
		bool urgent = bar->urg & 1 << i;

		if (hide_vacant && !active && !occupied && !urgent)
			continue;

		pixman_color_t *fg_color = urgent ? &urgent_fg_color : (active ? &active_fg_color : &inactive_fg_color);
		pixman_color_t *bg_color = urgent ? &urgent_bg_color : (active ? &active_bg_color : &inactive_bg_color);
			
		if (!hide_vacant && occupied)
			pixman_image_fill_boxes(PIXMAN_OP_SRC, foreground,
						fg_color, 1, &(pixman_box32_t){
							.x1 = xpos_left + boxs, .x2 = xpos_left + boxs + boxw,
							.y1 = boxs, .y2 = boxs + boxw
						});
		
		xpos_left = draw_text(tags[i], xpos_left, ypos, foreground, background, fg_color, bg_color,
				      bar->width, bar->height, bar->textpadding, NULL, 0, NULL, 0);
	}
	
	xpos_left = draw_text(bar->layout, xpos_left, ypos, foreground, background,
			      &inactive_fg_color, &inactive_bg_color, bar->width,
			      bar->height, bar->textpadding, NULL, 0, NULL, 0);

	uint32_t status_width = TEXT_WIDTH(bar->status, bar->width - xpos_left, bar->textpadding);
	draw_text(bar->status, bar->width - status_width, ypos, foreground,
		  background, &inactive_fg_color, &inactive_bg_color,
		  bar->width, bar->height, bar->textpadding,
		  bar->status_colors, bar->status_colors_l,
		  bar->status_buttons, bar->status_buttons_l);

	xpos_left = draw_text(bar->title, xpos_left, ypos, foreground, background,
			      bar->selmon ? &active_fg_color : &inactive_fg_color,
			      bar->selmon ? &active_bg_color : &inactive_bg_color,
			      bar->width - status_width, bar->height, bar->textpadding,
			      NULL, 0, NULL, 0);

	pixman_image_fill_boxes(PIXMAN_OP_SRC, background,
				bar->selmon ? &active_bg_color : &inactive_bg_color, 1,
				&(pixman_box32_t){
					.x1 = xpos_left, .x2 = bar->width - status_width,
					.y1 = 0, .y2 = bar->height
				});

	/* Draw background and foreground on bar */
	pixman_image_composite32(PIXMAN_OP_OVER, background, NULL, final, 0, 0, 0, 0, 0, 0, bar->width, bar->height);
	pixman_image_composite32(PIXMAN_OP_OVER, foreground, NULL, final, 0, 0, 0, 0, 0, 0, bar->width, bar->height);

	pixman_image_unref(foreground);
	pixman_image_unref(background);
	pixman_image_unref(final);
	
	munmap(data, bar->bufsize);

	wl_surface_attach(bar->wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(bar->wl_surface, 0, 0, bar->width, bar->height);
	wl_surface_commit(bar->wl_surface);

	return 0;
}

/* Layer-surface setup adapted from layer-shell example in [wlroots] */
static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
			uint32_t serial, uint32_t w, uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	
	Bar *bar = (Bar *)data;
	
	if (bar->configured && w == bar->width && h == bar->height)
		return;
	
	bar->width = w;
	bar->height = h;
	bar->stride = bar->width * 4;
	bar->bufsize = bar->stride * bar->height;
	bar->configured = true;

	draw_frame(bar);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
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
	Bar *bar = (Bar *)data;
	
	/* Is this necessary? */
	if (bar->xdg_output_name)
		free(bar->xdg_output_name);
	if (!(bar->xdg_output_name = strdup(name)))
		EDIE("strdup");
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
shell_command(char *command)
{
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", command, NULL);
		perror(" failed");
		exit(EXIT_SUCCESS);
	}
}

static void
pointer_enter(void *data, struct wl_pointer *pointer,
	      uint32_t serial, struct wl_surface *surface,
	      wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Seat *seat = (Seat *)data;

	Bar *bar;
	DL_FOREACH(bar_list, bar)
		if (bar->wl_surface == surface)
			break;
	seat->pointer_bar = bar;

	if (!cursor_image) {
		struct wl_cursor_theme *cursor_theme = wl_cursor_theme_load(NULL, 24, shm);
		cursor_image = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr")->images[0];
		cursor_surface = wl_compositor_create_surface(compositor);
		wl_surface_attach(cursor_surface, wl_cursor_image_get_buffer(cursor_image), 0, 0);
		wl_surface_commit(cursor_surface);
	}
	wl_pointer_set_cursor(pointer, serial, cursor_surface,
			      cursor_image->hotspot_x,
			      cursor_image->hotspot_y);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer,
	      uint32_t serial, struct wl_surface *surface)
{
	Seat *seat = (Seat *)data;
	
	seat->pointer_bar = NULL;
}

static void
pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
	       uint32_t time, uint32_t button, uint32_t state)
{
	Seat *seat = (Seat *)data;

	seat->pointer_button = state == WL_POINTER_BUTTON_STATE_PRESSED ? button : 0;
}

static void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
	       wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Seat *seat = (Seat *)data;

	seat->pointer_x = wl_fixed_to_int(surface_x);
	seat->pointer_y = wl_fixed_to_int(surface_y);
}

static void
pointer_frame(void *data, struct wl_pointer *pointer)
{
	Seat *seat = (Seat *)data;

	if (!seat->pointer_button || !seat->pointer_bar)
		return;

	for (uint32_t i = 0; i < seat->pointer_bar->status_buttons_l; i++) {\
		if (seat->pointer_button == seat->pointer_bar->status_buttons[i].button
		    && seat->pointer_x >= seat->pointer_bar->status_buttons[i].x1
		    && seat->pointer_x < seat->pointer_bar->status_buttons[i].x2) {
			shell_command(seat->pointer_bar->status_buttons[i].command);
			break;
		}
	}
	
	seat->pointer_button = 0;
}

static void
pointer_axis(void *data, struct wl_pointer *pointer,
	     uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *pointer,
		      uint32_t axis, int32_t discrete)
{
}

static void
pointer_axis_source(void *data, struct wl_pointer *pointer,
		    uint32_t axis_source)
{
}

static void
pointer_axis_stop(void *data, struct wl_pointer *pointer,
		  uint32_t time, uint32_t axis)
{
}

static void
pointer_axis_value120(void *data, struct wl_pointer *pointer,
		      uint32_t axis, int32_t discrete)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.axis = pointer_axis,
	.axis_discrete = pointer_axis_discrete,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_value120 = pointer_axis_value120,
	.button = pointer_button,
	.enter = pointer_enter,
	.frame = pointer_frame,
	.leave = pointer_leave,
	.motion = pointer_motion,
};

static void
seat_capabilities(void *data, struct wl_seat *wl_seat,
		  uint32_t capabilities)
{
	Seat *seat = (Seat *)data;
	
	uint32_t has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
	if (has_pointer && !seat->wl_pointer) {
		seat->wl_pointer = wl_seat_get_pointer(seat->wl_seat);
		wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
	} else if (!has_pointer && seat->wl_pointer) {
		wl_pointer_destroy(seat->wl_pointer);
		seat->wl_pointer = NULL;
	}
}

static void
seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void
show_bar(Bar *bar)
{
	bar->wl_surface = wl_compositor_create_surface(compositor);
	if (!bar->wl_surface)
		DIE("Could not create wl_surface");

	bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, bar->wl_surface, bar->wl_output,
								 ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, PROGRAM);
	if (!bar->layer_surface)
		DIE("Could not create layer_surface");
	zwlr_layer_surface_v1_add_listener(bar->layer_surface, &layer_surface_listener, bar);

	zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, bar->height);
	zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
					 (bar->bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, bar->height);
	wl_surface_commit(bar->wl_surface);

	bar->hidden = false;
}

static void
hide_bar(Bar *bar)
{
	zwlr_layer_surface_v1_destroy(bar->layer_surface);
	wl_surface_destroy(bar->wl_surface);

	bar->configured = false;
	bar->hidden = true;
}

static void
setup_bar(Bar *bar)
{
	bar->height = height;
	bar->textpadding = textpadding;
	bar->bottom = bottom;
	bar->hidden = hidden;

	snprintf(bar->layout, sizeof bar->layout, "[]=");
		
	bar->xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, bar->wl_output);
	if (!bar->xdg_output)
		DIE("Could not create xdg_output");
	zxdg_output_v1_add_listener(bar->xdg_output, &output_listener, bar);

	if (!bar->hidden)
		show_bar(bar);
}

static void
setup_seat(Seat *seat)
{
	wl_seat_add_listener(seat->wl_seat, &seat_listener, seat);
}

static void
handle_global(void *data, struct wl_registry *registry,
	      uint32_t name, const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
		output_manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2);
	} else if (!strcmp(interface, wl_output_interface.name)) {
		Bar *bar = malloc(sizeof(Bar));
		if (!bar)
			EDIE("malloc");
		memset(bar, 0, sizeof(Bar));
		bar->registry_name = name;
		bar->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
		DL_APPEND(bar_list, bar);
		if (ready)
			setup_bar(bar);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		Seat *seat = malloc(sizeof(Seat));
		if (!seat)
			EDIE("malloc");
		memset(seat, 0, sizeof(Seat));
		seat->registry_name = name;
		seat->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
		DL_APPEND(seat_list, seat);
		setup_seat(seat);
	}
}

static void
teardown_bar(Bar *bar)
{
	zxdg_output_v1_destroy(bar->xdg_output);
	if (!bar->hidden) {
		zwlr_layer_surface_v1_destroy(bar->layer_surface);
		wl_surface_destroy(bar->wl_surface);
	}
	if (bar->xdg_output_name)
		free(bar->xdg_output_name);
	if (bar->status_colors)
		free(bar->status_colors);
	if (bar->status_buttons)
		free(bar->status_buttons);
	free(bar);
}

static void
teardown_seat(Seat *seat)
{
	wl_seat_destroy(seat->wl_seat);
	free(seat);
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	do {
		Bar *bar;
		DL_FOREACH(bar_list, bar)
			if (bar->registry_name == name)
				break;
		if (!bar)
			break;
		DL_DELETE(bar_list, bar);
		teardown_bar(bar);
		return;
	} while(0);

	do {
		Seat *seat;
		DL_FOREACH(seat_list, seat)
			if (seat->registry_name == name)
				break;
		if (!seat)
			break;
		DL_DELETE(seat_list, seat);
		teardown_seat(seat);
	} while(0);
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
			EDIE("read");
		}
		if (rv == 0) {
			run_display = 0;
			return;
		}

		if ((len += rv) > stdinbuf_cap / 2)
			if (!(stdinbuf = realloc(stdinbuf, (stdinbuf_cap *= 2))))
				EDIE("realloc");
	}

	char *linebeg, *lineend;
	char *wordbeg, *wordend;
	
	for (linebeg = stdinbuf;
	     (lineend = memchr(linebeg, '\n', stdinbuf + len - linebeg));
	     linebeg = lineend) {
		*lineend++ = '\0';
		wordend = linebeg;

		ADVANCE_IF_LAST_CONT();

		Bar *bar;
		DL_FOREACH(bar_list, bar)
			if (bar->xdg_output_name)
				if (!strcmp(wordbeg, bar->xdg_output_name))
					break;
		if (!bar)
			continue;
		
		ADVANCE_IF_LAST_CONT();

		uint32_t val;
		if (!strcmp(wordbeg, "tags")) {
			ADVANCE_IF_LAST_CONT();
			if ((val = atoi(wordbeg)) != bar->ctags) {
				bar->ctags = val;
				bar->redraw = true;
			}
			ADVANCE_IF_LAST_CONT();
			if ((val = atoi(wordbeg)) != bar->mtags) {
				bar->mtags = val;
				bar->redraw = true;
			}
			ADVANCE_IF_LAST_CONT();
			/* skip sel */
			ADVANCE();
			if ((val = atoi(wordbeg)) != bar->urg) {
				bar->urg = val;
				bar->redraw = true;
			}
		} else if (!strcmp(wordbeg, "layout")) {
			if (strcmp(bar->layout, wordend) != 0) {
				snprintf(bar->layout, sizeof bar->layout, "%s", wordend);
				bar->redraw = true;
			}
		} else if (!strcmp(wordbeg, "title")) {
			if (strcmp(bar->title, wordend) != 0) {
				snprintf(bar->title, sizeof bar->title, "%s", wordend);
				bar->redraw = true;
			}
		} else if (!strcmp(wordbeg, "selmon")) {
			ADVANCE();
			if ((val = atoi(wordbeg)) != bar->selmon) {
				bar->selmon = val;
				bar->redraw = true;
			}
		}
	}
}

static void
set_top(Bar *bar)
{
	if (!bar->hidden) {
		zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
						 ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		bar->redraw = true;
	}
	bar->bottom = false;
}

static void
set_bottom(Bar *bar)
{
	if (!bar->hidden) {
		zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
						 ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		bar->redraw = true;
	}
	bar->bottom = true;
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

static void
set_status(Bar *bar, char *text)
{
	bar->status_colors_l = 0;
	bar->status_buttons_l = 0;

	size_t str_pos = 0;
	uint32_t codepoint;
	uint32_t state = UTF8_ACCEPT;

	StatusButton *left_button = NULL;
	StatusButton *middle_button = NULL;
	StatusButton *right_button = NULL;
	
	for (char *p = text; *p && str_pos < sizeof(bar->status) - 1; p++) {
		if (state == UTF8_ACCEPT && *p == '^') {
			p++;
			if (*p != '^') {
				char *arg, *end;
				if (!(arg = strchr(p, '(')) || !(end = strchr(arg + 1, ')')))
					continue;
				*arg++ = '\0';
				*end = '\0';
				
				if (!strcmp(p, "bg")) {
					StatusColor *status_color;
					ARRAY_APPEND(bar->status_colors, bar->status_colors_l, bar->status_colors_c, status_color);
					if (!*arg)
						status_color->color = inactive_bg_color;
					else
						parse_color(arg, &status_color->color);
					status_color->bg = true;
					status_color->start = bar->status + str_pos;
				} else if (!strcmp(p, "fg")) {
					StatusColor *status_color;
					ARRAY_APPEND(bar->status_colors, bar->status_colors_l, bar->status_colors_c, status_color);
					if (!*arg)
						status_color->color = inactive_fg_color;
					else
						parse_color(arg, &status_color->color);
					status_color->bg = false;
					status_color->start = bar->status + str_pos;
				} else if (!strcmp(p, "lm")) {
					if (left_button) {
						left_button->end = bar->status + str_pos;
						left_button = NULL;
					} else if (*arg) {
						ARRAY_APPEND(bar->status_buttons, bar->status_buttons_l, bar->status_buttons_c, left_button);
						left_button->button = BTN_LEFT;
						snprintf(left_button->command, sizeof left_button->command, "%s", arg);
						left_button->start = bar->status + str_pos;
					}
				} else if (!strcmp(p, "mm")) {
					if (middle_button) {
						middle_button->end = bar->status + str_pos;
						middle_button = NULL;
					} else if (*arg) {
						ARRAY_APPEND(bar->status_buttons, bar->status_buttons_l, bar->status_buttons_c, middle_button);
						middle_button->button = BTN_MIDDLE;
						snprintf(middle_button->command, sizeof middle_button->command, "%s", arg);
						middle_button->start = bar->status + str_pos;
					}
				} else if (!strcmp(p, "rm")) {
					if (right_button) {
						right_button->end = bar->status + str_pos;
						right_button = NULL;
					} else if (*arg) {
						ARRAY_APPEND(bar->status_buttons, bar->status_buttons_l, bar->status_buttons_c, right_button);
						right_button->button = BTN_RIGHT;
						snprintf(right_button->command, sizeof right_button->command, "%s", arg);
						right_button->start = bar->status + str_pos;
					}
				} 

				*--arg = '(';
				*end = ')';
				
				p = end;
				continue;
			}
		}

		bar->status[str_pos++] = *p;
		utf8decode(&state, &codepoint, *p);
	}

	bar->status[str_pos] = '\0';
}

static void
read_socket(void)
{
	int cli_fd;
	if ((cli_fd = accept(sock_fd, NULL, 0)) == -1)
		EDIE("accept");
	ssize_t len = recv(cli_fd, sockbuf, sizeof sockbuf - 1, 0);
	if (len == -1)
		EDIE("recv");
	close(cli_fd);
	if (len == 0)
		return;
	sockbuf[len] = '\0';

	do {
		char *wordbeg, *wordend;
		wordend = (char *)&sockbuf;

		ADVANCE_IF_LAST_BREAK();
		
		Bar *bar;
		bool all = false;
		
		if (!strcmp(wordbeg, "all")) {
			all = true;
		} else if (!strcmp(wordbeg, "selected")) {
			DL_FOREACH(bar_list, bar)
				if (bar->selmon)
					break;
		} else if (!strcmp(wordbeg, "first")) {
			bar = bar_list;
		} else {
			DL_FOREACH(bar_list, bar)
				if (bar->xdg_output_name)
					if (!strcmp(wordbeg, bar->xdg_output_name))
						break;
		}
		
		if (!all && !bar)
			break;

		ADVANCE();

		if (!strcmp(wordbeg, "status")) {
			if (!wordend)
				break;
			if (all) {
				DL_FOREACH(bar_list, bar) {
					set_status(bar, wordend);
					bar->redraw = true;
				}
			} else {
				set_status(bar, wordend);
				bar->redraw = true;
			}
		} else if (!strcmp(wordbeg, "show")) {
			if (all) {
				DL_FOREACH(bar_list, bar)
					if (bar->hidden)
						show_bar(bar);
			} else {
				if (bar->hidden)
					show_bar(bar);
			}
		} else if (!strcmp(wordbeg, "hide")) {
			if (all) {
				DL_FOREACH(bar_list, bar)
					if (!bar->hidden)
						hide_bar(bar);
			} else {
				if (!bar->hidden)
					hide_bar(bar);
			}
		} else if (!strcmp(wordbeg, "toggle-visibility")) {
			if (all) {
				DL_FOREACH(bar_list, bar)
					if (bar->hidden)
						show_bar(bar);
					else
						hide_bar(bar);
			} else {
				if (bar->hidden)
					show_bar(bar);
				else
					hide_bar(bar);
			}
		} else if (!strcmp(wordbeg, "set-top")) {
			if (all) {
				DL_FOREACH(bar_list, bar)
					if (bar->bottom)
						set_top(bar);
						
			} else {
				if (bar->bottom)
					set_top(bar);
			}
		} else if (!strcmp(wordbeg, "set-bottom")) {
			if (all) {
				DL_FOREACH(bar_list, bar)
					if (!bar->bottom)
						set_bottom(bar);
						
			} else {
				if (!bar->bottom)
					set_bottom(bar);
			}
		} else if (!strcmp(wordbeg, "toggle-location")) {
			if (all) {
				DL_FOREACH(bar_list, bar)
					if (bar->bottom)
						set_top(bar);
					else
						set_bottom(bar);
			} else {
				if (bar->bottom)
					set_top(bar);
				else
					set_bottom(bar);
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
		FD_SET(wl_fd, &rfds);
		FD_SET(STDIN_FILENO, &rfds);
		FD_SET(sock_fd, &rfds);

		wl_display_flush(display);

		if (select(MAX(sock_fd, wl_fd) + 1, &rfds, NULL, NULL, NULL) == -1)
			continue;

		if (FD_ISSET(wl_fd, &rfds))
			if (wl_display_dispatch(display) == -1)
				break;
		
		if (FD_ISSET(STDIN_FILENO, &rfds))
			read_stdin();

		if (FD_ISSET(sock_fd, &rfds))
			read_socket();

		Bar *bar;
		DL_FOREACH(bar_list, bar) {
			if (bar->redraw) {
				if (!bar->hidden)
					draw_frame(bar);
				bar->redraw = false;
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
	char *xdgruntimedir;
	struct sockaddr_un sock_address;
	Bar *bar, *bar2;
	Seat *seat, *seat2;

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
		} else if (!strcmp(argv[i], "-no-hide-vacant-tags")) {
			hide_vacant = false;
		} else if (!strcmp(argv[i], "-bottom")) {
			bottom = true;
		} else if (!strcmp(argv[i], "-no-bottom")) {
			bottom = false;
		} else if (!strcmp(argv[i], "-hidden")) {
			hidden = true;
		} else if (!strcmp(argv[i], "-no-hidden")) {
			hidden = false;
		} else if (!strcmp(argv[i], "-status-commands")) {
			status_commands = true;
		} else if (!strcmp(argv[i], "-no-status-commands")) {
			status_commands = false;
		} else if (!strcmp(argv[i], "-font")) {
			if (++i >= argc)
				DIE("Option -font requires an argument");
			fontstr = argv[i];
		} else if (!strcmp(argv[i], "-vertical-padding")) {
			if (++i >= argc)
				DIE("Option -vertical-padding requires an argument");
			vertical_padding = MAX(MIN(atoi(argv[i]), 100), 0);
		} else if (!strcmp(argv[i], "-active-fg-color")) {
			if (++i >= argc)
				DIE("Option -active-fg-color requires an argument");
			if (parse_color(argv[i], &active_fg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-active-bg-color")) {
			if (++i >= argc)
				DIE("Option -active-bg-color requires an argument");
			if (parse_color(argv[i], &active_bg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-inactive-fg-color")) {
			if (++i >= argc)
				DIE("Option -inactive-fg-color requires an argument");
			if (parse_color(argv[i], &inactive_fg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-inactive-bg-color")) {
			if (++i >= argc)
				DIE("Option -inactive-bg-color requires an argument");
			if (parse_color(argv[i], &inactive_bg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-urgent-fg-color")) {
			if (++i >= argc)
				DIE("Option -urgent-fg-color requires an argument");
			if (parse_color(argv[i], &urgent_fg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-urgent-bg-color")) {
			if (++i >= argc)
				DIE("Option -urgent-bg-color requires an argument");
			if (parse_color(argv[i], &urgent_bg_color) == -1)
				DIE("malformed color string");
		} else if (!strcmp(argv[i], "-tags")) {
			if (i + (int)LENGTH(tags) >= argc)
				DIE("Option -tags requires %lu arguments", LENGTH(tags));
			for (int j = 0; j < (int)LENGTH(tags); j++)
				tags[j] = argv[i + 1 + j];
			i += LENGTH(tags);
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
	height = font->height + vertical_padding * 2;
	
	/* Setup bars */
	DL_FOREACH(bar_list, bar)
		setup_bar(bar);
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
		EDIE("bind");
	if (listen(sock_fd, SOMAXCONN) == -1)
		EDIE("listen");
	fcntl(sock_fd, F_SETFD, FD_CLOEXEC | fcntl(sock_fd, F_GETFD));

	/* Set up signals */
	signal(SIGINT, sig_handler);
	signal(SIGHUP, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGCHLD, SIG_IGN);
	
	/* Run */
	ready = true;
	event_loop();

	/* Clean everything up */
	close(sock_fd);
	unlink(socketpath);
	
	zwlr_layer_shell_v1_destroy(layer_shell);
	zxdg_output_manager_v1_destroy(output_manager);
	
	DL_FOREACH_SAFE(bar_list, bar, bar2)
		teardown_bar(bar);
	DL_FOREACH_SAFE(seat_list, seat, seat2)
		teardown_seat(seat);
	
	fcft_destroy(font);
	fcft_fini();
	
	wl_shm_destroy(shm);
	wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);

	return 0;
}
