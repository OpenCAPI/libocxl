srcdir = $(PWD)
include Makefile.vars

OBJS = obj/afu.o obj/internal.o obj/irq.o obj/mmio.o obj/setup.o
CFLAGS += -I src/include -I uthash/src -I kernel/include -fPIC

# change VERS_LIB if new git tag
VERS_LIB = 0.1
LIBNAME   = libocxl.so.$(VERS_LIB)
# change VERS_SONAME only if library breaks backward compatibility.
# refer to file symver.map
VERS_SONAME=0
LIBSONAME = libocxl.so.$(VERS_SONAME)
SONAMEOPT = -Wl,-soname,$(LIBSONAME)

DOCDIR = docs

all: docs obj/$(LIBSONAME) obj/libocxl.so obj/libocxl.a

HAS_WGET = $(shell /bin/which wget > /dev/null 2>&1 && echo y || echo n)
HAS_CURL = $(shell /bin/which curl > /dev/null 2>&1 && echo y || echo n)

# Update this to test a single feature from the most recent header we require:
#CHECK_CXL_HEADER_IS_UP_TO_DATE = $(shell /bin/echo -e \\\#include $(1)\\\nvoid test\(struct cxl_afu_id test\)\; | \
#                 $(CC) $(CFLAGS) -Werror -x c -S -o /dev/null - > /dev/null 2>&1 && echo y || echo n)
#
#check_cxl_header:
#ifeq ($(call CHECK_CXL_HEADER_IS_UP_TO_DATE,"<misc/cxl.h>"),n)
#	mkdir -p include/misc
#ifeq (${HAS_WGET},y)
#	$(call Q,WGET include/misc/cxl.h, wget -O include/misc/cxl.h -q http://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/plain/include/uapi/misc/cxl.h)
#else ifeq (${HAS_CURL},y)
#	$(call Q,CURL include/misc/cxl.h, curl -o include/misc/cxl.h -s http://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/plain/include/uapi/misc/cxl.h)
#else
#	$(error 'cxl.h is non-existant or out of date, Download from http://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/plain/include/uapi/misc/cxl.h and place in ${PWD}/include/misc/cxl.h')
#endif
#endif

obj:
	mkdir obj

obj/libocxl.so: obj/$(LIBNAME)
	ln -sf $(LIBNAME) obj/libocxl.so

obj/$(LIBSONAME): obj/$(LIBNAME)
	ln -sf $(LIBNAME) obj/$(LIBSONAME)

obj/$(LIBNAME): obj $(OBJS) symver.map
	$(call Q,CC, $(CC) $(CFLAGS) $(LDFLAGS) -shared $(OBJS) -o obj/$(LIBNAME), obj/$(LIBNAME)) -Wl,--version-script symver.map $(SONAMEOPT)

obj/libocxl.a: $(OBJS)
	$(call Q,AR, ar rcs obj/libcxl.a $(OBJS), obj/libcxl.a)

include Makefile.rules

cppcheck:
	cppcheck --enable=all -j 4 -q  src/*.c src/include/libocxl.h
	
cppcheck-xml:
	cppcheck --enable=all -j 4 -q  src/*.c src/include/libocxl.h --xml-version=2 2>cppcheck.xml

precommit: clean all cppcheck
	astyle --style=linux --indent=tab=8 --max-code-length=120 src/*.c src/*.h src/include/*.h

docs:
	rm -rf $(DOCDIR)
	$(call Q,DOCS-MAN, doxygen Doxyfile-man,)
	$(call Q,DOCS-HTML, doxygen Doxyfile-html,)

clean:
	rm -rf obj docs

install: all
	mkdir -p $(libdir)
	mkdir -p $(includedir)
	mkdir -p $(mandir)/man3
	mkdir -p $(datadir)/libocxl/search
	install -m 0755 obj/$(LIBNAME) $(libdir)/
	cp -d obj/libocxl.so obj/$(LIBSONAME) $(libdir)/
	install -m 0644 src/include/libocxl.h  $(includedir)/
	install -m 0644 -D docs/man/man3/* $(mandir)/man3
	install -m 0644 -D docs/html/*.* $(datadir)/libocxl
	install -m 0644 -D docs/html/search/* $(datadir)/libocxl/search


.PHONY: clean all install docs precommit cppcheck cppcheck-xml
