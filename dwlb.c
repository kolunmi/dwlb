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
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"
#include "xdg-output-protocol.h"

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
#define USAGE \
	"usage: dwlb [OPTIONS]\n"					\
	"	-status	[OUTPUT] [TEXT]		send status text to dwlb\n" \
	"	-hide-vacant-tags		do not display empty and inactive tags\n" \
	"	-font [FONT]			specify a font\n"	\
	"	-text-color [COLOR]		specify text color\n"	\
	"	-active-color [COLOR]		specify color to indicate active tags or monitors\n" \
	"	-inactive-color [COLOR]		specify color to indicate inactive tags or monitors\n" \
	"	-urg-text-color [COLOR]		specify text color on urgent tags\n" \
	"	-urg-bg-color [COLOR]		specify color of urgent tags\n"	\
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
	int selmon;
	char *layout;
	char *title;
	char status[512];

	bool redraw;

	Bar *prev, *next;
};

static int sock_fd;
static char socketdir[256];
static char *socketpath = NULL;
static char sockbuf[768];
static char stdinbuf[BUFSIZ];

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zxdg_output_manager_v1 *output_manager;

static uint32_t height;
static uint32_t textpadding;

static bool run_display = true;
static bool ready = false;
static bool hide_vacant = false;
static Bar *bars = NULL;

static char *tags[9] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };
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

static uint32_t
draw_text(char *text,
	  uint32_t xpos,
	  uint32_t ypos,
	  pixman_image_t *foreground,
	  pixman_image_t *background,
	  pixman_color_t *fgcolor,
	  pixman_color_t *bgcolor,
	  uint32_t max_x,
	  uint32_t height,
	  uint32_t padding)
{
	uint32_t codepoint;
	uint32_t state = UTF8_ACCEPT;
	uint32_t ixpos = xpos;
	uint32_t nxpos;
	uint32_t lastcp = 0;
	pixman_image_t *fgfill = pixman_image_create_solid_fill(fgcolor);
	
	if (!*text)
		return xpos;

	if ((nxpos = xpos + padding) > max_x)
		return xpos;
	
	xpos = nxpos;
	
	for (char *p = text; *p; p++) {
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

		if ((nxpos = xpos + x_kern + glyph->advance.x) > max_x) {
			if (!lastcp)
				return ixpos;
			break;
		}
		
		xpos += x_kern;
		lastcp = codepoint;

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

		/* increment pen position */
		xpos = nxpos;
		ypos += glyph->advance.y;
	}

	if (state != UTF8_ACCEPT)
		fprintf(stderr, "malformed UTF-8 sequence\n");

	xpos = MIN(xpos + padding, max_x);

	if (background && bgcolor)
		pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
					bgcolor, 1, &(pixman_box32_t){
						.x1 = ixpos,
						.x2 = xpos,
						.y1 = 0,
						.y2 = height
					});

	pixman_image_unref(fgfill);

	return xpos;
}

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
	
	/* Fill bar with background color */
	pixman_image_fill_boxes(PIXMAN_OP_SRC, bar, b->selmon ? &activecolor : &inactivecolor, 1,
				&(pixman_box32_t) {.x1 = 0, .x2 = b->width, .y1 = 0, .y2 = b->height});
	
	/* Text background and foreground layers */
	pixman_image_t *background_left = pixman_image_create_bits(PIXMAN_a8r8g8b8, b->width, b->height, NULL, b->width * 4);
	pixman_image_t *foreground_left = pixman_image_create_bits(PIXMAN_a8r8g8b8, b->width, b->height, NULL, b->width * 4);
	pixman_image_t *background_right = pixman_image_create_bits(PIXMAN_a8r8g8b8, b->width, b->height, NULL, b->width * 4);
	pixman_image_t *foreground_right = pixman_image_create_bits(PIXMAN_a8r8g8b8, b->width, b->height, NULL, b->width * 4);
	pixman_image_t *foreground_title = pixman_image_create_bits(PIXMAN_a8r8g8b8, b->width, b->height, NULL, b->width * 4);
	
	/* Draw on images */
	uint32_t xpos_left = 0;
	uint32_t xpos_right;
	uint32_t ypos = (b->height + font->ascent - font->descent) / 2;
	
	uint32_t boxs = font->height / 9;
	uint32_t boxw = font->height / 6 + 2;

	for (uint32_t i = 0; i < 9; i++) {
		bool active = b->mtags & 1 << i;
		bool occupied = b->ctags & 1 << i;
		bool urgent = b->urg & 1 << i;

		if (hide_vacant && !active && !occupied && !urgent)
			continue;
		
		if (!hide_vacant && occupied)
			pixman_image_fill_boxes(PIXMAN_OP_SRC, foreground_left,
						&textcolor, 1, &(pixman_box32_t){
							.x1 = xpos_left + boxs,
							.x2 = xpos_left + boxs + boxw,
							.y1 = boxs,
							.y2 = boxs + boxw
						});
		
		if (urgent)
			xpos_left = draw_text(tags[i], xpos_left, ypos, foreground_left, background_left,
					      &urgtextcolor, &urgbgcolor, b->width, b->height, b->textpadding);
		else
			xpos_left = draw_text(tags[i], xpos_left, ypos, foreground_left, background_left,
					      &textcolor, active ? &activecolor : &inactivecolor, b->width, b->height, b->textpadding);
	}
	xpos_left = draw_text(b->layout, xpos_left, ypos, foreground_left, background_left,
			      &textcolor, &inactivecolor, b->width, b->height, b->textpadding);

	xpos_right = draw_text(b->status, 0, ypos, foreground_right, background_right,
			       &textcolor, &inactivecolor, b->width, b->height, b->textpadding);
	if (xpos_right > b->width)
		xpos_right = b->width;
	
	draw_text(b->title, 0, ypos, foreground_title, NULL,
		  &textcolor, NULL, b->width, b->height, b->textpadding);

	/* Draw background and foreground on bar */
	pixman_image_composite32(PIXMAN_OP_OVER, foreground_title, NULL, bar, 0, 0, 0, 0, xpos_left, 0, b->width, b->height);
	pixman_image_composite32(PIXMAN_OP_OVER, background_right, NULL, bar, 0, 0, 0, 0, b->width - xpos_right, 0, b->width, b->height);
	pixman_image_composite32(PIXMAN_OP_OVER, foreground_right, NULL, bar, 0, 0, 0, 0, b->width - xpos_right, 0, b->width, b->height);
	pixman_image_composite32(PIXMAN_OP_OVER, background_left, NULL, bar, 0, 0, 0, 0, 0, 0, b->width, b->height);
	pixman_image_composite32(PIXMAN_OP_OVER, foreground_left, NULL, bar, 0, 0, 0, 0, 0, 0, b->width, b->height);

	pixman_image_unref(foreground_left);
	pixman_image_unref(background_left);
	pixman_image_unref(foreground_right);
	pixman_image_unref(background_right);
	pixman_image_unref(foreground_title);
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

static void
cleanup(void)
{
	if (socketpath)
		unlink(socketpath);
}

static struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

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

static struct zxdg_output_v1_listener output_listener = {
	.name = output_name,
	.description = output_description,
	.done = output_done,
	.logical_position = output_logical_position,
	.logical_size = output_logical_size
};

static void
setup_bar(Bar *b)
{
	b->height = height;
	b->textpadding = textpadding;
		
	b->layout = "[]=";
	b->title = "";
		
	b->xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, b->wl_output);
	if (!b->xdg_output)
		CLEANUP_DIE("Could not create xdg_output");
	zxdg_output_v1_add_listener(b->xdg_output, &output_listener, b);
		
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
					 ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	wl_surface_commit(b->wl_surface);
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
	ssize_t len = read(STDIN_FILENO, stdinbuf, sizeof stdinbuf);
	if (len == -1)
		CLEANUP_EDIE("read");
	if (len == 0) {
		run_display = 0;
		return;
	}

	char *end = (char *)&stdinbuf + len;
	char *linebeg, *lineend;
	char *wordbeg, *wordend;
	
	for (linebeg = (char *)&stdinbuf;
	     (lineend = memchr(linebeg, '\n', end - linebeg));
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
		
		if (!strcmp(wordbeg, "tags")) {
			ADVANCE_IF_LAST_CONT();
			b->ctags = atoi(wordbeg);
			ADVANCE_IF_LAST_CONT();
			b->mtags = atoi(wordbeg);
			ADVANCE_IF_LAST_CONT();
			/* skip sel */
			ADVANCE();
			b->urg = atoi(wordbeg);
			b->redraw = true;
		} else if (!strcmp(wordbeg, "layout")) {
			b->layout = wordend;
			b->redraw = true;
		} else if (!strcmp(wordbeg, "title")) {
			b->title = wordend;
			b->redraw = true;
		} else if (!strcmp(wordbeg, "selmon")) {
			ADVANCE();
			b->selmon = atoi(wordbeg);
			b->redraw = true;
		}
	}
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

		ADVANCE_IF_LAST_BREAK();

		if (!strcmp(wordbeg, "status")) {
			if (all) {
				DL_FOREACH(bars, b) {
					snprintf(b->status, sizeof b->status, "%s", wordend);
					b->redraw = true;
				}
			} else {
				snprintf(b->status, sizeof b->status, "%s", wordend);
				b->redraw = true;
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
				draw_frame(b);
				b->redraw = false;
			}
		}
	}
}

static void
client_send_command(struct sockaddr_un *sock_address,
		    const char *output,
		    const char *cmd,
		    const char *data)
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
		} else if (!strcmp(argv[i], "-hide-vacant-tags")) {
			hide_vacant = true;
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
	Bar *b;
	DL_FOREACH(bars, b)
		setup_bar(b);
	wl_display_roundtrip(display);
	
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
	signal(SIGPIPE, SIG_IGN);

	/* Run */
	ready = true;
	event_loop();

	/* Clean everything up */
	close(sock_fd);
	unlink(socketpath);
	
	zwlr_layer_shell_v1_destroy(layer_shell);
	zxdg_output_manager_v1_destroy(output_manager);
	
	Bar *t;
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
