-include .config

INSTALL ?= install

# Installation paths

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CFLAGS += -c -MMD -Wall 

LDLIBS_ASOUND ?= -lasound
LDLIBS_OPUS ?= -lopus
LDLIBS_ORTP ?= -lortp

LDLIBS += $(LDLIBS_ASOUND) $(LDLIBS_OPUS) $(LDLIBS_ORTP)

.PHONY:		all install dist clean

all:		rx tx

rx:		rx.o rx_start.o device.o sched.o multi.o admin.o

tx:		tx.o rx_start.o device.o sched.o multi.o admin.o

install:	rx tx
		$(INSTALL) -d $(DESTDIR)$(BINDIR)
		$(INSTALL) rx tx $(DESTDIR)$(BINDIR)

dist:
		mkdir -p dist
		V=$$(git describe) && \
		git archive --prefix="trx-$$V/" HEAD | \
			gzip > "dist/trx-$$V.tar.gz"

clean:
		rm -f *.o *.d tx rx

-include *.d
