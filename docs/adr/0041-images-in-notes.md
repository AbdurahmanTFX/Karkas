# 0041 — Images in Markdown notes: shown, dropped, pasted; stb_image as a dependency

Status: accepted (continues ADR 0040; adds a dependency beyond section 18: stb_image)

## Context

The user asked (2026-11-21) for image support in notes. Asked back, the user chose: images are shown in
the rendered views (inspector, Both, Rendered), added by dropping a file on the note and by Ctrl+V from
the clipboard; only local images are shown — a web image is never fetched (the server would see the
user's address and when the note was opened).

## Decision

Markdown (kk_core):
- `![alt](src "title")` as before; `![alt|300](src)` — an Obsidian size after the alt text (width in
  pixels; `300x200` takes the width); `![[file.png|300]]` — an Obsidian embed of an image file is an image.
  An embed of anything else (`![[Note]]`) stays a `[[link]]`. `md::images(text)` lists the images outside
  code; `md::is_image_file` knows .png .jpg .jpeg .gif .bmp .webp .svg .avif.
- `MediaFinder` finds the file an image shows: `![](src)` relative to the note's folder (`/…` from the
  project root, `%20` decoded); `![[name]]` in the note's folder, at the root, else the file of that name
  anywhere in the project nearest to the note (hidden folders and build trees skipped; the index of names
  is made once, when first needed). URLs give nothing.
- The schema loader keeps a Markdown note's local images (`Block::media`); `karkas check` gives **W09** for
  one whose file is not there. Embeds of images are no `[[links]]` (no W08) and their words no mentions.

The dependency: **stb_image** (nothings/stb, public domain, one header), pinned to commit 1da9ade (v2.34,
2026-11-19 — the newest, because its last fixes are bounds checks in the BMP and GIF readers); the
system's header is used when there is one (Arch: `stb`). Our own C file compiles only PNG, JPEG, GIF and
BMP from memory (`STBI_ONLY_*`, `STBI_NO_STDIO`, at most 16384 px a side), so less code reads the bytes of
files we did not write. Alternatives: libpng + libjpeg-turbo (two libraries for two formats), a decoder
of our own (inflate and JPEG are a lot of code to get safe), Wuffs (C, a code generator, larger).

karkas-gui:
- `ImageCache` (images.cpp): a thread of its own finds the file (MediaFinder), reads it (at most 64 MB),
  decodes it (image_decode.cpp) and shrinks it by a whole factor to at most 2048 px on the long side; the
  UI thread makes the OpenGL texture at the start of a frame and the worker wakes it. Every 2 s files
  that changed are read again and missing ones looked for again. WebP, SVG and AVIF are found but not
  decoded: "this format is not shown".
- The view (markdown_view.cpp): an image that can be drawn takes lines of its own, as wide as its file
  (times the UI scale) or as asked by `|300`, never wider than the text; while it decodes, a placeholder;
  a missing or unreadable image is its alt text with a warning sign; a web image is a link (to the
  browser). A click on an image opens the file in its program; its tooltip gives the file and its size.
- Dropping files (GLFW's drop callback → `drop.cpp` → the next frame): into the Markdown note whose tab
  is open, else the note selected. A file inside the project is linked where it is; one from outside is
  copied next to the note — an image into `images/`, another file into `files/` (a name taken gets
  `-2`, a same file is reused). An image becomes `![stem](images/x.png)`, another file `[name](files/x)`;
  a path with spaces goes between `<` and `>`. The links go in at the editor's cursor, on lines of their
  own (at the end when only the view is shown).
- Ctrl+V in a Markdown note's tab with an image and no text in the clipboard: the worker asks
  `wl-paste --list-types` (Wayland) or `xclip -t TARGETS` (X11), takes image/png, jpeg, gif or bmp, and
  saves `images/paste-YYYYMMDD-HHMMSS.<ext>` next to the note; its link goes in at the cursor. The tools
  run without a shell, stderr dropped, killed after 3–5 s. Text in the clipboard is pasted as text.
- The script of headless checks has `drop <path>` and `key ctrl+v`.

## Consequences

- Links put in by a drop or a paste are no step of the editor's undo (ImGuiColorTextEdit's InsertText
  keeps none); Ctrl+Z undoes the typing before them.
- Files dropped on a tile that is a window of the desktop are not seen (the callback is on the main window).
- No animation (a GIF shows its first frame), no image scaling better than a box filter and linear
  sampling, no images on the schema's blocks (their preview lines show the alt text).
- W09 is worked out when the note's text changes: an image file added later clears it at the next edit.
- Tests: sizes and embeds parsed, embeds of notes stay links, images listed outside code with lines; the
  finder (relative, root, `%20`, by name nearest, hidden folders, URLs); the loader's media, no W08 for an
  embed, W09 for missing ones, no mention inside an embed; a PNG written by the GUI's writer decodes to
  its pixels, shrinks by the box filter, broken bytes give a reason. Checked under Xvfb: PNG and JPEG (an
  embed by name with `|300`) in the inspector and the views, warnings for a missing file and WebP, a web
  image as a link, a file dropped (copied into images/, linked at the cursor, saved), an image pasted
  from the X11 clipboard (saved as paste-….png, the same bytes, linked).
