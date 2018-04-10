srcdir = $(PWD)
include Makefile.vars

OBJS = obj/afu.o obj/internal.o obj/irq.o obj/mmio.o obj/setup.o
TEST_OBJS = testobj/afu.o testobj/internal.o testobj/irq.o testobj/mmio.o testobj/setup.o
CFLAGS += -I src/include -I kernel/include -fPIC -D_FILE_OFFSET_BITS=64

VERS_LIB = $(VERSION_MAJOR).$(VERSION_MINOR)
LIBNAME   = libocxl.so.$(VERS_LIB)
LIBSONAME = libocxl.so.$(VERSION_MAJOR)
SONAMEOPT = -Wl,-soname,$(LIBSONAME)

DOCDIR = docs

all: check_ocxl_header obj/$(LIBSONAME) obj/libocxl.so obj/libocxl.a sampleobj/memcpy

HAS_WGET = $(shell /bin/which wget > /dev/null 2>&1 && echo y || echo n)
HAS_CURL = $(shell /bin/which curl > /dev/null 2>&1 && echo y || echo n)

# Update this to test a single feature from the most recent header we require:
CHECK_OCXL_HEADER_IS_UP_TO_DATE = $(shell /bin/echo -e \\\#include $(1)\\\nvoid test\(struct ocxl_ioctl_metadata test\)\; | \
	$(CC) $(CFLAGS) -Werror -x c -S -o /dev/null - > /dev/null 2>&1 && echo y || echo n)

check_ocxl_header:
ifeq ($(call CHECK_OCXL_HEADER_IS_UP_TO_DATE,'<misc/ocxl.h>'),n)
	mkdir -p kernel/include/misc
ifeq (${HAS_WGET},y)
	$(call Q,WGET kernel/include/misc/ocxl.h, wget -O kernel/include/misc/ocxl.h -q http://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/plain/include/uapi/misc/ocxl.h)
else ifeq (${HAS_CURL},y)
	$(call Q,CURL kernel/include/misc/ocxl.h, curl -o kernel/include/misc/ocxl.h -s http://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/plain/include/uapi/misc/ocxl.h)
else
	$(error 'ocxl.h is non-existant or out of date, Download from http://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/plain/include/uapi/misc/ocxl.h and place in ${PWD}/kernel/include/misc/ocxl.h')
endif
endif

obj:
	mkdir obj

obj/libocxl.so: obj/$(LIBNAME)
	ln -sf $(LIBNAME) obj/libocxl.so

obj/$(LIBSONAME): obj/$(LIBNAME)
	ln -sf $(LIBNAME) obj/$(LIBSONAME)

obj/$(LIBNAME): $(OBJS) symver.map
	$(call Q,CC, $(CC) $(CFLAGS) $(LDFLAGS) -shared $(OBJS) -o obj/$(LIBNAME), obj/$(LIBNAME)) -Wl,--version-script symver.map $(SONAMEOPT)

obj/libocxl.a: $(OBJS)
	$(call Q,AR, $(AR) rcs obj/libocxl.a $(OBJS), obj/libocxl.a)

sampleobj/memcpy: sampleobj/memcpy.o-memcpy
	$(call Q,CC, $(CC) $(CFLAGS) $(LDFLAGS) -o sampleobj/memcpy sampleobj/memcpy.o-memcpy obj/libocxl.a, sampleobj/memcpy)

testobj:
	mkdir testobj

sampleobj:
	mkdir sampleobj

testobj/libocxl.a: $(TEST_OBJS)
	$(call Q,AR, $(AR) rcs testobj/libocxl-temp.a $(TEST_OBJS), testobj/libocxl-temp.a)
	$(call Q,STATIC_SYMS, $(NM) testobj/libocxl-temp.a | grep ' t ' | grep -v __ | cut -d ' ' -f 3 > testobj/static-syms)
	$(call Q,STATIC_PROTOTYPES, perl -n static-prototypes.pl src/*.c >testobj/static.h)
	$(call Q,OBJCOPY, $(OBJCOPY) --globalize-symbols=testobj/static-syms testobj/libocxl-temp.a testobj/libocxl.a, obj/libocxl.a)

testobj/unittests: testobj/unittests.o-test testobj/virtocxl.o-test
	$(call Q,CC, $(CC) $(CFLAGS) $(LDFLAGS) -o testobj/unittests testobj/unittests.o-test testobj/virtocxl.o-test testobj/libocxl.a -lfuse -lpthread, testobj/unittests)

test: check_ocxl_header testobj/unittests
	sudo testobj/unittests

valgrind: testobj/unittests
	sudo valgrind testobj/unittests

include Makefile.rules

cppcheck:
	cppcheck --enable=all -j 4 -q  src/*.c src/include/libocxl.h
	
cppcheck-xml:
	cppcheck --enable=all -j 4 -q  src/*.c src/include/libocxl.h --xml-version=2 2>cppcheck.xml

precommit: clean all docs cppcheck
	astyle --style=linux --indent=tab=8 --max-code-length=120 src/*.c src/*.h src/include/*.h samples/*/*.c
	$(call Q, SYMVER-CHECK, nm obj/$(LIBNAME) | grep ' t ocxl' && (echo "Symbols are missing from symver.map" && exit 1) || true)

docs:
	rm -rf $(DOCDIR)
	$(call Q,DOCS-MAN, doxygen Doxyfile-man,)
	cd docs/man/man3 && \
		ls | grep -vi ocxl | xargs rm
	$(call Q,DOCS-HTML, doxygen Doxyfile-html,)

clean:
	rm -rf obj testobj sampleobj docs

install: all docs
	mkdir -p $(DESTDIR)$(libdir)
	mkdir -p $(DESTDIR)$(includedir)
	mkdir -p $(DESTDIR)$(mandir)/man3
	mkdir -p $(DESTDIR)$(docdir)/libocxl/search
	$(INSTALL) -m 0755 obj/$(LIBNAME) $(DESTDIR)$(libdir)/
	ln -s $(LIBNAME) $(DESTDIR)$(libdir)/$(LIBSONAME)
	ln -s $(LIBNAME) $(DESTDIR)$(libdir)/libocxl.so
	$(INSTALL) -m 0644 src/include/libocxl.h  $(DESTDIR)$(includedir)/
	$(INSTALL) -m 0644 -D docs/man/man3/* $(DESTDIR)$(mandir)/man3
	$(INSTALL) -m 0644 -D docs/html/*.* $(DESTDIR)$(docdir)/libocxl
	$(INSTALL) -m 0644 -D docs/html/search/* $(DESTDIR)$(docdir)/libocxl/search

.PHONY: clean all install docs precommit cppcheck cppcheck-xml check_ocxl_header
