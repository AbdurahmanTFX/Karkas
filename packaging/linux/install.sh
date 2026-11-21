#!/bin/sh
# Installs this Karkas package (docs/adr/0046) for the current user (~/.local) or into PREFIX:
#   ./install.sh                 → ~/.local
#   sudo PREFIX=/usr/local ./install.sh
# What was installed is listed in $PREFIX/share/karkas/installed-<package>.txt; uninstall.sh removes it.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
prefix=${PREFIX:-"$HOME/.local"}
package=$(basename "$here")
package=${package%%-[0-9]*}
list="$prefix/share/karkas/installed-$package.txt"
mkdir -p "$prefix/share/karkas"
: > "$list"
put() {  # put <file relative to the package> [mode]
  mkdir -p "$(dirname "$prefix/$1")"
  cp -f "$here/$1" "$prefix/$1"
  chmod "${2:-644}" "$prefix/$1"
  echo "$prefix/$1" >> "$list"
}
for f in "$here"/bin/*; do put "bin/$(basename "$f")" 755; done
if [ -d "$here/share" ]; then
  (cd "$here" && find share -type f) | while read -r f; do put "$f"; done
fi
desktop="$prefix/share/applications/karkas.desktop"
if [ -f "$desktop" ]; then
  # The menu of the desktop may not have $prefix/bin in its PATH: the full path of the program.
  sed -i "s|^Exec=karkas-gui|Exec=$prefix/bin/karkas-gui|" "$desktop"
  command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$prefix/share/applications" || true
  # A cache of the icons only where there is one already (a new cache would hide icons other programs add later);
  # else the folder's time tells the desktop to look again.
  if [ -f "$prefix/share/icons/hicolor/icon-theme.cache" ] && command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t "$prefix/share/icons/hicolor" || true
  else
    touch "$prefix/share/icons/hicolor"
  fi
fi
cp -f "$here/uninstall.sh" "$prefix/share/karkas/uninstall-$package.sh" 2>/dev/null && chmod 755 "$prefix/share/karkas/uninstall-$package.sh" || true
echo "Karkas ($package) installed into $prefix"
case ":$PATH:" in *":$prefix/bin:"*) ;; *) echo "note: $prefix/bin is not in PATH";; esac
