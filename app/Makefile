TARGET = pspdx
OBJS = main.o gui/screen.o gui/entropy_screen.o logic/entropy.o \
       network/https.o update/catalog.o install/install.o \
       install/zipread.o util/runtime.o

WOLFSSL_PREFIX = $(CURDIR)/wolfssl-psp/prefix

# -I. only: every local include names its directory, so the layer a header
# comes from is visible at the include line.
CFLAGS = -O2 -G0 -Wall -I. -I$(WOLFSSL_PREFIX)/include
ASFLAGS = $(CFLAGS)

LIBDIR = $(WOLFSSL_PREFIX)/lib
LIBS = -lwolfssl -lcjson -lz -lpspsemaphore_660 -lpsppower \
       -lpspnet_resolver -lpspnet_apctl -lpspnet_inet -lpspnet \
       -lpsputility -lpspdebug -lpspdisplay -lpspge -lpspctrl

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSPDX

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
