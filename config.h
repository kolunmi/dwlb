#define HEX_COLOR(hex)				\
	{ .red   = ((hex >> 24) & 0xff) * 257,	\
	  .green = ((hex >> 16) & 0xff) * 257,	\
	  .blue  = ((hex >> 8) & 0xff) * 257,	\
	  .alpha = (hex & 0xff) * 257 }

// use ipc functionality
static bool ipc = true;
// initially hide all bars
static bool hidden = false;
// initially draw all bars at the bottom
static bool bottom = false;
// hide vacant tags
static bool hide_vacant = false;
// vertical pixel padding above and below text
static uint32_t vertical_padding = 4;
// allow in-line color commands in status text
static bool status_commands = true;
// center title text
static bool center_title = false;
// use title space as status text element
static bool custom_title = false;
// title color use active colors
static bool active_color_title = true;
// scale
static uint32_t buffer_scale = 1;
// font
static char *fontstr = "monospace:bold:size=12.5";
// tag names
static char *tags_names[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

// set 16-bit colors for bar
// use either pixman_color_t struct or HEX_COLOR macro for 8-bit colors
#if 1
static pixman_color_t active_fg_color = HEX_COLOR(0xffffffff);
// static pixman_color_t active_bg_color = HEX_COLOR(0x005577ff);
static pixman_color_t active_bg_color = HEX_COLOR(0x1177AAff);
static pixman_color_t occupied_fg_color = HEX_COLOR(0xeeeeeeff);
static pixman_color_t occupied_bg_color = HEX_COLOR(0x222222ff);
static pixman_color_t inactive_fg_color = HEX_COLOR(0xbbbbbbff);
static pixman_color_t inactive_bg_color = HEX_COLOR(0x222222ff);
static pixman_color_t urgent_fg_color = HEX_COLOR(0x222222ff);
static pixman_color_t urgent_bg_color = HEX_COLOR(0xeeeeeeff);
static pixman_color_t middle_bg_color = HEX_COLOR(0x222222ff);
static pixman_color_t middle_bg_color_selected = HEX_COLOR(0x1177AAff);
// static pixman_color_t middle_bg_color_selected = HEX_COLOR(0x005577ff);
#else
static pixman_color_t active_fg_color   = HEX_COLOR(0xeeeeeeff); // selfgcolor
static pixman_color_t active_bg_color   = HEX_COLOR(0x2f133fff); // selbgcolor

static pixman_color_t occupied_fg_color = HEX_COLOR(0xeeeeeeff); // selfgcolor
static pixman_color_t occupied_bg_color = HEX_COLOR(0x2f133fff); // selbgcolor

static pixman_color_t inactive_fg_color = HEX_COLOR(0xbbbbbbff); // normfgcolor
static pixman_color_t inactive_bg_color = HEX_COLOR(0x1c1021ff); // normbgcolor

static pixman_color_t urgent_fg_color   = HEX_COLOR(0x1c1021ff); // dark text
static pixman_color_t urgent_bg_color   = HEX_COLOR(0x4f335fff); // selbordercolor

static pixman_color_t middle_bg_color            = HEX_COLOR(0x1c1021ff); // normbgcolor
static pixman_color_t middle_bg_color_selected  = HEX_COLOR(0x2f133fff); // selbgcolor
#endif
