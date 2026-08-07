#!/bin/sh
#
# Builds R Markdown and installs it into the Deskbar tray. Run on Haiku itself.
#
# Usage:
#   ./install.sh              build and install
#   ./install.sh --uninstall  remove the tray item and the binary
#   ./install.sh --build-only build in place, install nothing

set -e

APP="R Markdown"
SRC="RMarkdown.cpp"
RDEF="RMarkdown.rdef"
LIBS="-lbe -ltracker -llocalestub -lroot"

APPS_DIR="$HOME/config/non-packaged/apps"
MENU_DIR="$HOME/config/non-packaged/data/deskbar/menu/Applications"
DESKTOP_DIR="$HOME/Desktop"

cd "$(dirname "$0")"

if [ "$(uname -s)" != "Haiku" ]; then
	echo "This builds a Haiku application; run it on Haiku." >&2
	exit 1
fi

if [ "$1" = "--uninstall" ]; then
	rm -f "$APPS_DIR/$APP" "$MENU_DIR/$APP" "$DESKTOP_DIR/$APP"
	echo "Removed $APP."
	exit 0
fi

echo "Compiling..."
g++ -O2 -o "$APP" "$SRC" $LIBS

# The signature has to be a resource, not an attribute: Deskbar stores the
# replicant's "add_on" signature and loads the binary back through the roster
# on every boot, and mimeset -F drops a signature attached with addattr.
echo "Attaching resources..."
rc -o RMarkdown.rsrc "$RDEF"
xres -o "$APP" RMarkdown.rsrc
mimeset -f "$APP"
rm -f RMarkdown.rsrc

if [ "$1" = "--build-only" ]; then
	echo "Built ./$APP (not installed)."
	exit 0
fi

echo "Installing..."
mkdir -p "$APPS_DIR" "$MENU_DIR" "$DESKTOP_DIR"

# Never overwrite a copy that is still running: replacing the file under a
# live process invalidates its code pages and it dies in unrelated places.
quit application/x-vnd.RMarkdown >/dev/null 2>&1 || true
sleep 2
# The bracket keeps this grep from matching its own command line.
if ps | grep -q "[R] Markdown"; then
	echo "$APP is still running -- close it and run this again." >&2
	exit 1
fi

cp -f "$APP" "$APPS_DIR/$APP"
ln -sf "$APPS_DIR/$APP" "$MENU_DIR/$APP"
ln -sf "$APPS_DIR/$APP" "$DESKTOP_DIR/$APP"
rm -f "$APP"

echo "Installed to $APPS_DIR/$APP"
echo "Find it in Deskbar -> Applications -> $APP, or on the Desktop."
