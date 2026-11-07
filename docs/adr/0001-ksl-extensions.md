# 0001 — Small KSL extensions and clarifications

Status: proposed (needs the human's approval)

The grammar of 4.5 left a few cases open. Implemented choices:

1. `dirs blocks=. api=include impl=src` in project.ksl: folder conventions, so that
   Karkas can describe its own repository (libs/*, apps/*) as required in 19.1.
2. `task <text>` in @ticket for feature/change/question tickets (`symptom` is for bugs).
3. `use` is allowed in shared blocks (shared.log uses shared.types); the table of 4.4
   lists it only for leaf/entry. A `use` of a non-shared block is reported as E03.
4. hist: at most 10 entries plus one folded line `hist summary <n> earlier entries T-a..T-b`.
5. Comments: `#` starts a comment only at line start or after whitespace (so `C#` stays
   text). Free text that contains ` #` or starts with `"` is written as one quoted string.
6. Trail lines: `L<n> <role padded to 13> attrs` (as in the 10.2 example); `answer at=<block>
   "<text>"` and `note <text>` lines; a contract-change proposal of @report goes into a
   `--- diff` section.
7. Syntax errors use codes S01–S07 (unknown record kind, unknown directive, bad argument,
   repeated directive, bad header id, unknown section) with file:line:col.
