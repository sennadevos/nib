#!/usr/bin/env bash
# Copy the Qt/Chromium runtime out of the toolbox next to the binary, so nib
# runs on an atomic host with nothing installed on the base system.
#
# Run INSIDE the toolbox: ./bundle.sh ~/.local/lib/nib
set -euo pipefail

DEST="${1:?usage: bundle.sh DESTDIR}"
HOSTLIB=/run/host/usr/lib64          # toolbox sees the host image here
QT_LIBEXEC=/usr/lib64/qt6/libexec
QT_SHARE=/usr/share/qt6
QT_PLUGINS=/usr/lib64/qt6/plugins
QT_QML=/usr/lib64/qt6/qml

mkdir -p "$DEST"/{lib,libexec,resources,translations,plugins,qml}

# --- shared libraries -------------------------------------------------------
# Walk the dependency closure of the binary and the Chromium helper. Bundle
# every Qt library (nib owns its Qt, so a host Qt update cannot break it) plus
# anything the host image simply does not have.
declare -A seen=()
work=()

queue() {
	local f=$1
	[ -e "$f" ] || return 0
	local key; key=$(basename "$f")
	[ -n "${seen[$key]:-}" ] && return 0
	seen[$key]=1
	work+=("$f")
}

queue ./nib
queue "$QT_LIBEXEC/QtWebEngineProcess"

copied=0
while [ ${#work[@]} -gt 0 ]; do
	cur="${work[0]}"; work=("${work[@]:1}")
	while read -r name _arrow path _rest; do
		[ "${path:-}" = "" ] && continue
		[ -e "$path" ] || continue
		case "$name" in
		linux-vdso*|ld-linux*) continue ;;
		esac
		# Qt libs: always bundle. Others: only if the host lacks them, so the
		# graphics/driver stack keeps coming from the host.
		if [[ "$name" == libQt6* || "$name" == libqt6* ]]; then
			:
		elif [ -e "$HOSTLIB/$name" ]; then
			continue
		fi
		if [ -z "${seen[$name]:-}" ]; then
			seen[$name]=1
			cp -Ln "$path" "$DEST/lib/$name" 2>/dev/null || cp -L "$path" "$DEST/lib/$name"
			copied=$((copied + 1))
			work+=("$path")
		fi
	done < <(ldd "$cur" 2>/dev/null | sed 's/^[[:space:]]*//')
done
echo "bundled $copied shared libraries"

# --- Chromium helper, resources, locales ------------------------------------
install -m755 "$QT_LIBEXEC/QtWebEngineProcess" "$DEST/libexec/QtWebEngineProcess"
cp -a "$QT_SHARE/resources/." "$DEST/resources/"
cp -a "$QT_SHARE/translations/qtwebengine_locales" "$DEST/translations/" 2>/dev/null || true

# --- Qt plugins and QML modules --------------------------------------------
# QtWebEngineWidgets renders through Quick, so the Quick QML modules must come
# along with the platform/wayland plugins.
for d in platforms wayland-shell-integration wayland-decoration-client \
         wayland-graphics-integration-client xcbglintegrations imageformats \
         iconengines tls platforminputcontexts; do
	[ -d "$QT_PLUGINS/$d" ] && cp -a "$QT_PLUGINS/$d" "$DEST/plugins/"
done
for m in QtQml QtQuick QtCore QtWebEngine QtWebChannel QtPositioning; do
	[ -e "$QT_QML/$m" ] && cp -a "$QT_QML/$m" "$DEST/qml/"
done
# module registry files sit at the qml root
cp -a "$QT_QML"/*.qmltypes "$DEST/qml/" 2>/dev/null || true

echo "bundle at $DEST ($(du -sh "$DEST" | cut -f1))"
