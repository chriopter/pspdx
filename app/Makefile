TARGET = pspdx
OBJS = main.o gui/screen.o gui/entropy_screen.o gui/gfx.o gui/font.o \
       gui/image.o gui/lattice.o gui/title.o gui/preview.o gui/icons.o gui/marks.o gui/shell.o logic/entropy.o network/https.o network/bench.o \
       update/catalog.o update/assets.o update/sync.o install/install.o \
       install/zipread.o audio/audio.o audio/synth.o audio/music.o \
       audio/cues.o video/mp4.o video/psmf.o video/player.o util/runtime.o

WOLFSSL_PREFIX = $(CURDIR)/wolfssl-psp/prefix

# Which build this is: the tag if the checkout is on one, the commit if it is
# not. It is shown in the info band and written into the record PSPDX keeps
# about itself, and that record's version is held against the catalog's, which
# is the release tag with its "v" taken off -- so the "v" comes off here too,
# or the two strings would never agree and the client would offer itself an
# update for ever. The Docker build mounts app/ alone and sees no .git, so it
# falls back to "dev"; dev/release passes the version in from the host.
PSPDX_DESCRIBE := $(shell git describe --tags --always 2>/dev/null || echo dev)
PSPDX_VERSION ?= $(PSPDX_DESCRIBE:v%=%)

# -I. only: every local include names its directory, so the layer a header
# comes from is visible at the include line.
# -MMD writes a .d next to every .o so that a header change rebuilds what
# includes it; build.mak on its own only looks at the .c.
# EXTRA_CFLAGS is for a test build: tools/localcat points the client at a
# catalog on the host and trusts the host's throwaway CA through it.
CFLAGS = -O2 -G0 -Wall -I. -I$(WOLFSSL_PREFIX)/include -MMD -MP \
         -DPSPDX_VERSION=\"$(PSPDX_VERSION)\" $(EXTRA_CFLAGS)
ASFLAGS = $(CFLAGS)

LIBDIR = $(WOLFSSL_PREFIX)/lib
# intraFont draws through the GE, so it has to come before -lpspgu; libpng
# leans on the zlib that is already here for the package archives.
LIBS = -lwolfssl -lcjson -lintrafont -lpng -lz -lm \
       -lpspgu -lpspgum -lpspmpeg -lpspaudio -lpspsemaphore_660 -lpsppower \
       -lpspnet_resolver -lpspnet_apctl -lpspnet_inet -lpspnet \
       -lpsputility -lpspdebug -lpspdisplay -lpspge -lpspctrl

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSPDX
# The picture the XMB shows beside the title, and the one the catalog lists
# the client under: the same file, 144x80, as every PSP package carries.
PSP_EBOOT_ICON = assets/icon0.png

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# After build.mak, so its all: stays the default goal.
-include $(OBJS:.o=.d)
