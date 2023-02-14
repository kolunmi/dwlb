// bar properties
static bool hidden = false;
static bool bottom = false;
static bool hide_vacant = false;

// define the number of tags and the tag names, if the number of tags is
//  greater than TAGSLEN they will not be displayed, each tag also needs a name
#define TAGSLEN 9
static char *tags[TAGSLEN] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

// set colors for bar
// TODO: explain the formatting? or at least how a hex color code would be translated
static pixman_color_t activecolor = { .red = 0x0000, .green = 0x5555, .blue = 0x7777, .alpha = 0xffff, };
static pixman_color_t inactivecolor = { .red = 0x2222, .green = 0x2222, .blue = 0x2222, .alpha = 0xffff, };
static pixman_color_t textcolor = { .red = 0xeeee, .green = 0xeeee, .blue = 0xeeee, .alpha = 0xffff, };
static pixman_color_t urgbgcolor = { .red = 0xeeee, .green = 0xeeee, .blue = 0xeeee, .alpha = 0xffff, };
static pixman_color_t urgtextcolor = { .red = 2222, .green = 0x2222, .blue = 0x2222, .alpha = 0xffff, };
