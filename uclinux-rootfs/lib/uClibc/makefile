include $(ROOTDIR)/config.arch

unexport MAKEFILE_LIST

BOGUS_TARGETS		:= FORCE makefile $(ROOTDIR)/config.arch

.PHONY: all
all::
	$(MAKE) -f Makefile

.PHONY: romfs
romfs::
	mkdir -p $(ROMFSDIR)/lib
	cp -f lib/*.so* $(ROMFSDIR)/lib/

# These targets must not be passed through to the original Makefile
.PHONY: $(BOGUS_TARGETS)
$(BOGUS_TARGETS)::

# Everything else (maybe including clean, distclean) does get passed through
%:: FORCE
	$(MAKE) -f Makefile $@
