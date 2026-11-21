Karkas — schema-first environment: the project is a schema of blocks, agents write the code inside them.

This package is the application: karkas-gui (the window) and karkas (the command line it runs for the
chat, the run panel and the MCP server). Both must stay in one folder.

Linux
  Run in place:         ./bin/karkas-gui
  Install for yourself: ./install.sh            (into ~/.local, with a menu entry and the icon)
  Install for everyone: sudo PREFIX=/usr/local ./install.sh
  Remove:               ./uninstall.sh          (the same PREFIX)
  Needs: libcurl, SQLite 3 (with FTS5), OpenGL 3.3.

Windows
  Run karkas-gui.exe. Keep karkas.exe next to it.

Agents need Claude Code (the "claude" command) or an API key in the settings.
