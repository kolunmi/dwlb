# Dwlb Version
VERSION = 0.0.1

# Paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

# includes and libs
FCFT_LIB = `pkg-config --libs fcft`
FCFT_INC  = `pkg-config --cflags fcft`

PIXMAN_LIB = `pkg-config --libs pixman-1`
PIXMAN_INC  = `pkg-config --cflags pixman-1`

# Wayland Protocols
WAYLAND_PROTOCOLS = `pkg-config --variable=pkgdatadir wayland-protocols`
WAYLAND_SCANNER = `pkg-config --variable=wayland_scanner wayland-scanner`

WAYLAND_LIB = `pkg-config --libs wayland-client wayland-cursor` 
WAYLAND_INC = `pkg-config --cflags wayland-client wayland-cursor` 

INCS = ${FCFT_INC} ${PIXMAN_INC} ${WAYLAND_INC}
LIBS = ${FCFT_LIB} ${PIXMAN_LIB} ${WAYLAND_LIB}

# flags
CFLAGS = -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -g -Os ${INCS}
LDFLAGS = ${LIBS}

# compiler
CC = cc
