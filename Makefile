#
CURL	?= $(shell if test -f /usr/bin/curl ; then echo "curl -H Pragma: -O -R -S --fail --show-error" ; fi)
WGET	?= $(shell if test -f /usr/bin/wget ; then echo "wget -nd -m" ; fi)
CLIENT	?= $(if $(CURL),$(CURL),$(if $(WGET),$(WGET)))
AWK	= awk
SHA1SUM	= sha1sum
SED	= sed

# this is passed on the command line as the full path to <build>/SPECS/kernel.spec
SPECFILE = kernel-2.6.spec

# Thierry - when called from within the build, PWD is /build
PWD=$(shell pwd)

# get nevr from specfile.
ifndef NAME
NAME := $(shell rpm $(RPMDEFS) $(DISTDEFS) -q --qf "%{NAME}\n" --specfile $(SPECFILE) | head -1)
endif
ifndef EPOCH
EPOCH := $(shell rpm $(RPMDEFS) $(DISTDEFS) -q --qf "%{EPOCH}\n" --specfile $(SPECFILE) | head -1 | sed 's/(none)//')
endif
ifeq ($(EPOCH),(none))
override EPOCH := ""
endif
ifndef VERSION
VERSION := $(shell rpm $(RPMDEFS) $(DISTDEFS) -q --qf "%{VERSION}\n" --specfile $(SPECFILE)| head -1)
endif
ifndef RELEASE
RELEASE := $(shell rpm $(RPMDEFS) $(DISTDEFS) -q --qf "%{RELEASE}\n" --specfile $(SPECFILE)| head -1)
endif

define get_sources_sha1
$(shell cat sources 2>/dev/null | awk 'gensub("^.*/", "", 1, $$2) == "$@" { print $$1; exit; }')
endef
define get_sources_url1
$(shell cat sources 2>/dev/null | awk 'gensub("^.*/", "", 1, $$2) == "$@" { print $$2; exit; }')
endef
define get_sources_url2
$(shell cat sources 2>/dev/null | awk 'gensub("^.*/", "", 1, $$2) == "$@" { print $$3; exit; }')
endef
SOURCEFILES := $(shell cat sources 2>/dev/null | awk '{ print gensub("^.*/", "", 1, $$2) }')
SOURCE_RPM := $(firstword $(SOURCEFILES))

sources: $(SOURCEFILES) $(TARGETS)

$(SOURCEFILES): #FORCE
	@if [ ! -e "$@" ] ; then \
	 { echo Using primary; echo "$(CLIENT) $(get_sources_url1)" ; $(CLIENT) $(get_sources_url1) ; } || \
	 { echo Using secondary; echo "$(CLIENT) $(get_sources_url2)" ; $(CLIENT) $(get_sources_url2) ; } ; fi
	@if [ ! -e "$@" ] ; then echo "Could not download source file: $@ does not exist" ; exit 1 ; fi
	@if test "$$(sha1sum $@ | awk '{print $$1}')" != "$(get_sources_sha1)" ; then \
	    echo "sha1sum of the downloaded $@ does not match the one from 'sources' file" ; \
	    echo "Local copy: $$(sha1sum $@)" ; \
	    echo "In sources: $$(grep $@ sources)" ; \
	    exit 1 ; \
	else \
	    ls -l $@ ; \
	fi

download-sources:
	@for i in $(SOURCES); do \
		if [ ! -e "$${i##*/}" ]; then \
			echo "$(CLIENT) $$i"; \
			$(CLIENT) $$i; \
		fi; \
	done

replace-sources:
	rm -f sources
	@$(MAKE) new-sources

new-sources: download-sources
	@for i in $(SOURCES); do \
		echo "$(SHA1SUM) $$i >> sources"; \
		$(SHA1SUM) $${i##*/} | $(AWK) '{ printf "%s  %s\n", $$1, "'"$$i"'" }' >> sources; \
	done

PREPARCH ?= noarch
RPMDIRDEFS = --define "_sourcedir $(PWD)/SOURCES" --define "_builddir $(PWD)" --define "_srcrpmdir $(PWD)" --define "_rpmdir $(PWD)"
trees: sources
	rpmbuild $(RPMDIRDEFS) $(RPMDEFS) --nodeps -bp --target $(PREPARCH) $(SPECFILE)

# use the stock source rpm, unwrap it,
# copy the downloaded material
# install our own specfile and patched patches
# and patch configs for IPV6
# then rewrap with rpm
srpm: sources
	mkdir -p SOURCES SRPMS
	(cd SOURCES; rpm2cpio ../$(SOURCE_RPM) | cpio -diu; \
	 cp ../$(notdir $(SPECFILE)) . ; cp ../linux-*.patch .; cp ../config-vserver . ; cp ../config-planetlab .; \
	 for downloaded in $(SOURCEFILES) ; do cp ../$$downloaded . ; done ; \
	 cat config-vserver >> config-generic ; \
	 cat config-planetlab >> config-generic)
	./modify_rh_config.sh
	./rpmmacros.sh
	export HOME=$(shell pwd) ; rpmbuild $(RPMDIRDEFS) $(RPMDEFS) --nodeps -bs $(SPECFILE)

TARGET ?= $(shell uname -m)
rpm: sources
	rpmbuild $(RPMDIRDEFS) $(RPMDEFS) --nodeps --target $(TARGET) -bb $(SPECFILE)

clean:
	rm -f *.rpm
	rm -rf BUILDROOT SOURCES SPECS SRPMS kernel-2.6.32 tmp
