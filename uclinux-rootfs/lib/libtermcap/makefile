include $(ROOTDIR)/config.arch

TOOL_DIR_PREFIX=$(shell dirname $(shell which $(CC)))/..

ifneq (,$(findstring linux,$(CROSS)))
	TARGET=$(CROSS:linux-=linux)
	TARGET_ARCH ?= $(CROSS:-linux-=)
else
	ifneq (,$(findstring uclibc,$(CROSS)))
		TARGET_ARCH ?= $(CROSS:-uclibc-=)
		TARGET=$(TARGET_ARCH)-linux
	else
		echo "FIXME: Cannot determine target architecture"
		exit 99
	endif
endif
UCLIBC_INC = -I$(ROOTDIR)/uClibc/include
LINUX_INC=-I$(ROOTDIR)/$(LINUXDIR)/include
GCC_INC=-I$(TOOL_DIR_PREFIX)/$(TARGET)/include
PREFIX=$(ROOTDIR)/romfs
# THT: Order is important, as we want to use the uClibc headers first,
# and the headers from gcc last.
# BITS_PER_LONG is defined under #ifdef __KERNEL__ in 2.6 headers, so we define it here.
MY_CFLAGS +=

CC=$(TARGET)-gcc
LD=$(TARGET)-ld
AR=$(TARGET)-ar

CFLAGS=-Os -I. $(UCLIBC_INC) $(LINUX_INC) $(GCC_INC)

ARFLAGS=ucvr

all:
	make -f Makefile

clean:
	make -f Makefile clean

distclean:
	make -f Makefile distclean

romfs:
	$(ROMFSINST) -e CONFIG_LIB_LIBTERMCAP /lib/libtermcap.so
	$(ROMFSINST) -e CONFIG_LIB_LIBTERMCAP -s libtermcap.so \
		/lib/libtermcap.so.2
