TARGET = pspdx
OBJS = main.o https.o install.o zipread.o

WOLFSSL_PREFIX = $(CURDIR)/wolfssl-psp/prefix

CFLAGS = -O2 -G0 -Wall -I$(WOLFSSL_PREFIX)/include
ASFLAGS = $(CFLAGS)

LIBDIR = $(WOLFSSL_PREFIX)/lib
LIBS = -lwolfssl -lcjson -lz -lpspsemaphore_660 -lpsppower \
       -lpspnet_resolver -lpspnet_apctl -lpspnet_inet -lpspnet \
       -lpsputility -lpspdebug -lpspdisplay -lpspge -lpspctrl

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSPDX

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
