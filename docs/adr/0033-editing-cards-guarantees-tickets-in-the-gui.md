# 0033 — Editing cards, guarantees and tickets in the GUI

Status: accepted (amends 10.1 "tickets are append-only", 14.6 editing on the picture, 14.7)

## Context

The user asked (2026-11-20) to edit the generated tickets, the KSL card and the guarantees of a block
right in karkas-gui. Until now a card was edited in an external editor ("Card" button) or by the
fixed operations on the picture; tickets were read-only, and `TicketStore::save` refuses any change
to the task, symptom, original facts, trail or result (10.1: the protocol between agents is
append-only).

## Decision

- Card: the inspector's CARD (KSL) section has "Edit": the card becomes a text field (monospace);
  "Check and save" turns the text into an edit (`agents::op_replace_card`): it must parse, keep its
  `@block` id (renaming stays the Rename operation, which moves folders and references), and becomes
  the lines it removes and adds (a multiset difference of the canonical directive lines; comments stay
  as they are, ports keep their order unless a line changes). It then goes the way of every edit on
  the picture (14.6): the KSL diff preview, apply only if the schema checks pass, undo. The editor
  closes when the card on disk changes after a save; a change from elsewhere while editing is shown
  with "Reload".
- Guarantees: the GUARANTEES section of a leaf or shared block (also when it has none yet) has "Edit
  guarantees": one line each, add, remove; `agents::op_set_invariants` replaces the `inv` lines the
  same way (preview, checks, undo). Groups and entries have no guarantees of their own (3.8).
- Tickets: the selected ticket has "Edit": the whole ticket file as text; "Check and save" calls
  `core::TicketStore::edit_by_human`: it must parse as a ticket with the same id, a known kind and
  status, and is refused while an agent holds the ticket's lock (the file is written under that lock,
  canonically). The append-only rule of 10.1 keeps binding the agents (`save`, `append`); the human
  owns the ticket and may reword the task, correct facts, drop trail lines or reopen it. The agents
  read it as written at their next run. Errors show above the text; the editor closes when saved.

## Consequences

- No undo for ticket edits (tickets are not the schema; the editor shows the change before saving
  and "Cancel" drops it). A human may remove trail lines the orchestrator relied on; that is the
  human's choice, like closing a ticket by hand.
- Tests: `edit_by_human` (symptom and facts changed by a human while `save` still refuses it, id,
  status, parse errors and the lock refused, nothing written on refusal); `op_replace_card` (preview
  lines, order kept, undo, unchanged / broken / another id / schema-breaking refused);
  `op_set_invariants` (changed, removed, groups refused, unchanged refused). GUI checked under Xvfb:
  a guarantee typed into the card, previewed, applied (the editor closes, GUARANTEES 4); a fact typed
  into a ticket and saved.
