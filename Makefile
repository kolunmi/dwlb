include config.mk

# Protocol source files
PROTO_SRC = xdg-shell-protocol.c \
	xdg-output-unstable-v1-protocol.c \
	wlr-layer-shell-unstable-v1-protocol.c \
	dwl-ipc-unstable-v2-protocol.c

PROTO_OBJ = ${PROTO_SRC:.c=.o}

SRC = dwlb.c
OBJ = ${SRC:.c=.o} ${PROTO_OBJ}

all: dwlb

# Protocol header generation
xdg-shell-protocol.h:
	${WAYLAND_SCANNER} client-header \
		${WAYLAND_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	${WAYLAND_SCANNER} private-code \
		${WAYLAND_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml $@

xdg-output-unstable-v1-protocol.h:
	${WAYLAND_SCANNER} client-header \
		${WAYLAND_PROTOCOLS}/unstable/xdg-output/xdg-output-unstable-v1.xml $@

xdg-output-unstable-v1-protocol.c:
	${WAYLAND_SCANNER} private-code \
		${WAYLAND_PROTOCOLS}/unstable/xdg-output/xdg-output-unstable-v1.xml $@

wlr-layer-shell-unstable-v1-protocol.h:
	${WAYLAND_SCANNER} client-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@

wlr-layer-shell-unstable-v1-protocol.c:
	${WAYLAND_SCANNER} private-code \
		protocols/wlr-layer-shell-unstable-v1.xml $@

dwl-ipc-unstable-v2-protocol.h:
	${WAYLAND_SCANNER} client-header \
		protocols/dwl-ipc-unstable-v2.xml $@

dwl-ipc-unstable-v2-protocol.c:
	${WAYLAND_SCANNER} private-code \
		protocols/dwl-ipc-unstable-v2.xml $@

# Protocol object dependencies
xdg-shell-protocol.o: xdg-shell-protocol.h
xdg-output-unstable-v1-protocol.o: xdg-output-unstable-v1-protocol.h
wlr-layer-shell-unstable-v1-protocol.o: wlr-layer-shell-unstable-v1-protocol.h
dwl-ipc-unstable-v2-protocol.o: dwl-ipc-unstable-v2-protocol.h

# Main object dependencies

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk

config.h:
	cp config.def.h $@

dwlb.o: utf8.h config.h \
	xdg-shell-protocol.h \
	xdg-output-unstable-v1-protocol.h \
	wlr-layer-shell-unstable-v1-protocol.h \
	dwl-ipc-unstable-v2-protocol.h

dwlb: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f dwlb ${OBJ} \
		xdg-shell-protocol.h xdg-shell-protocol.c \
		xdg-output-unstable-v1-protocol.h xdg-output-unstable-v1-protocol.c \
		wlr-layer-shell-unstable-v1-protocol.h wlr-layer-shell-unstable-v1-protocol.c \
		dwl-ipc-unstable-v2-protocol.h dwl-ipc-unstable-v2-protocol.c \

install: all
	mkdir -p ${PREFIX}/bin
	cp -f dwlb ${PREFIX}/bin
	chmod 755 ${PREFIX}/bin/dwlb
	mkdir -p ${MANPREFIX}/man1
	cp -f dwlb.1 > ${MANPREFIX}/man1/dwlb.1
	chmod 644 ${MANPREFIX}/man1/dwlb.1


uninstall:
	rm -f ${PREFIX}/bin/dwlb \
		${MANPREFIX}/man1/dwlb.1

.PHONY: all clean dist install uninstall
