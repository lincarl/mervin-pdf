# RPM %post: refresh the desktop + icon caches so Mervin shows up in menus and
# the "Open with" list right after install. Best-effort (caches may be absent on
# minimal/server systems).
update-desktop-database -q /usr/share/applications 2>/dev/null || true
gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor 2>/dev/null || true
