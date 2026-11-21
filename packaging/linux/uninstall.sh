#!/bin/sh
# Removes what install.sh put into PREFIX (default ~/.local), by its list.
set -eu
prefix=${PREFIX:-"$HOME/.local"}
here=$(cd "$(dirname "$0")" && pwd)
package=$(basename "$here")
case "$package" in karkas) package=$(basename "$0" .sh); package=${package#uninstall-};; esac
package=${package%%-[0-9]*}
list="$prefix/share/karkas/installed-$package.txt"
[ -f "$list" ] || { echo "nothing installed: no $list"; exit 1; }
while read -r f; do rm -f "$f"; done < "$list"
rm -f "$list" "$prefix/share/karkas/uninstall-$package.sh"
rmdir "$prefix/share/karkas" 2>/dev/null || true
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$prefix/share/applications" 2>/dev/null || true
echo "Karkas ($package) removed from $prefix"
