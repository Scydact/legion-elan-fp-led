PREFIX   ?= /usr/local
LIBDIR   ?= $(PREFIX)/lib64
UNITDIR  ?= /etc/systemd/system/fprintd.service.d
MODE     ?= inject

CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
USB_LIBS   := $(shell pkg-config --libs libusb-1.0)

SHIM := elan-led-shim.so

all: $(SHIM)

$(SHIM): src/elan-led-shim.c
	$(CC) $(CFLAGS) -fPIC -shared $(USB_CFLAGS) -o $@ $< -ldl $(USB_LIBS)

install: $(SHIM)
	install -D -m 0755 $(SHIM) $(DESTDIR)$(LIBDIR)/$(SHIM)
	-command -v restorecon >/dev/null && restorecon -v $(DESTDIR)$(LIBDIR)/$(SHIM)
	install -d $(DESTDIR)$(UNITDIR)
	sed -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@MODE@|$(MODE)|' systemd/elan-led.conf.in \
		> $(DESTDIR)$(UNITDIR)/elan-led.conf
	-systemctl daemon-reload
	-systemctl try-restart fprintd.service

uninstall:
	rm -f $(DESTDIR)$(UNITDIR)/elan-led.conf $(DESTDIR)$(LIBDIR)/$(SHIM)
	-rmdir $(DESTDIR)$(UNITDIR) 2>/dev/null
	-systemctl daemon-reload
	-systemctl try-restart fprintd.service

clean:
	rm -f $(SHIM)

.PHONY: all install uninstall clean
