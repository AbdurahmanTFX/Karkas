# 0002 — Built-in Sugiyama layout when Graphviz is missing

Status: implemented; Graphviz path not verified here

Graphviz is not installed on the development machine and could not be installed (no root;
building downloaded sources was not allowed). kk_layout has the ILayoutEngine interface
planned in 14.2 with two engines:

- GraphvizEngine (cgraph + gvc, dot, rankdir=LR) — compiled only when pkg-config finds
  libgvc and libcgraph; it reports "run once: sudo dot -c" when dot is unknown.
- SugiyamaEngine — built in, deterministic, used otherwise and as a fallback on errors.

To enable Graphviz: `sudo pacman -S graphviz`, reconfigure (`cmake --preset dev`) and
rebuild; the status line of karkas-gui shows the engine in use. The Graphviz code was
written against the documented cgraph/gvc API but has not been compiled here.
