PREFIX  ?= $(HOME)/.local
LIBDIR   = $(PREFIX)/lib/nib
PKGS     = Qt6Widgets Qt6WebEngineWidgets Qt6WebChannel
MOC     ?= /usr/lib64/qt6/libexec/moc

CXXFLAGS += -std=c++17 -O2 -Wall -Wextra -fPIC $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))
# Find the bundled runtime next to the binary: bin/nib -> lib/nib/lib.
# --disable-new-dtags emits DT_RPATH rather than DT_RUNPATH: glibc applies
# DT_RUNPATH only to an object's own direct dependencies, so the bundled Qt
# libraries' dependencies would go unresolved.
LDFLAGS  += -Wl,--disable-new-dtags \
            -Wl,-rpath,'$$ORIGIN/../lib/nib/lib' \
            -Wl,-rpath,'$$ORIGIN/../lib/nib'

nib: main.cpp main.moc
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ main.cpp $(LDLIBS)

main.moc: main.cpp
	$(MOC) $< -o $@

# Copy the Qt/Chromium runtime out of the toolbox so the binary is
# self-contained on the host. Run inside the toolbox.
bundle: nib
	./bundle.sh $(LIBDIR)

install: nib
	install -Dm755 nib $(DESTDIR)$(PREFIX)/bin/nib
	install -Dm644 nib.desktop $(DESTDIR)$(PREFIX)/share/applications/nib.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nib
	rm -f $(DESTDIR)$(PREFIX)/share/applications/nib.desktop
	rm -rf $(LIBDIR)

# Offscreen so no window ever appears on the running session. Never run the
# GUI suite against a desktop someone is using: it steals focus, and
# focus-dependent results (clipboard) turn into phantom failures.
test: nib
	./test.sh

clean:
	rm -f nib main.moc

.PHONY: bundle install uninstall clean test
