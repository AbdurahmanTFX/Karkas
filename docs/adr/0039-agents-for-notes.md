# 0039 — Agents for notes: an assistant that reads, answers and proposes; links without a model

Status: accepted (continues ADR 0038; amends 9 agents for notes projects)

## Context

ADR 0038 left the agents of notes for the next stage. The user described the use (2026-11-21): a folder
where a plan or notes are developed; the AI supplements them, looks through them, suggests something and
links them — Karkas then has two uses, programming and notes. The user asked to continue with the agents.

## Decision

Without a model first (principle P7):
- Mentions without a link: a note's text names another note's or topic's title in plain words — whole
  words, any case, outside `[[links]]`, titles of 3+ letters (`core::find_mentions`). `code::unlinked_mentions`
  (in a note), `mentions_of` (of a note in others) and `link_mention` (the words put in brackets; the id
  before them when the title is ambiguous). karkas-gui lists them in the inspector of the selected note
  with a "Link" button each; the assistant gets them as its `mentions` tool.

The notes assistant (`Engine::notes`, kk_agents notes_agent):
- One agent run per request, through the same agent loop as every role: the tier of `[roles] notes`
  (medium by default; [blocks] / [levels] pins apply), the journal, costs, mode api or mcp, stopping.
  The system prompt `templates/prompts/notes.txt`; the stable context is the overview of the notes (topics,
  notes with ids, titles, first words and links; at most 200 notes, then "search_notes finds them"); the
  task is the human's words and the note or topic it is about.
- Tools: `notes_overview`, `read_note`, `search_notes`, `mentions`, `write_note` (the whole new text),
  `new_note`, `new_topic`, `finish(answer, summary)`. Reads see the run's own edits; a write to a note
  made in the run goes into that note.
- The model writes nothing: its edits become a proposal N-NNNN (`.karkas/proposals/N-NNNN.json`: the
  request, the scope, the summary, each edit with the text it was written over). Approval applies all
  or nothing — topics, new notes (card and note.txt), texts — and refuses when a note changed since or
  an id was taken; rejection keeps it as rejected. `[agents] auto_approve` applies it at once. A run that
  fails before any edit is an error; a run stopped at its limit keeps the edits it made as a proposal.
- `karkas notes "<request>" [--block id]`, `karkas proposal list | show | approve | reject`. In a notes
  project `karkas task` (and so the MCP tool kk_task) goes to the assistant; `karkas debug` is refused.
- A notes project made by `karkas init` gets a karkas.toml with the models and `[roles] notes`.

karkas-gui:
- The chat comes back in a notes project as the "Notes assistant": a message is `karkas notes "..."`,
  about all notes or the selected note or topic (its title shown), with `/notes` and `/proposal` commands;
  its answers carry Show / Approve / Reject for each N-NNNN they name. Auto applies proposals at once.
- The side bar's "AI proposals" (the changes view of code projects): the proposals, newest first, with
  waits / approved / rejected; the selected one shows the request, the summary and a diff note by note
  (`notes_proposal_preview`: new topics and notes, texts as line diffs, "changed since" when the note
  moved on), with Approve / Reject. The activity badge counts the pending ones.
- The inspector of a note: "Ask" (the chat limited to it) and "Mentions without a link".
- The Agents and Costs tabs, the agents' mode and the cost in the status bar show in notes projects too.

## Consequences

- Undo of an approved proposal is not offered yet: the files can be restored from the texts kept in the
  proposal (`base`), or from git.
- One run per request; no tickets, descents or escalation for notes. A larger plan ("sort all notes into
  topics") fits into one run only as far as its step limit goes; the proposal keeps what was done.
- Tests: mentions (word boundaries, case, links left out, short titles, link_mention with the words kept),
  the tool host (overview, search, mentions, read of own edits, new ids, errors, nothing written), a
  request answered with a proposal and approved (the prompt, the overview and the scope reached the model,
  the files only after the approval, the links of the new note), a note changed since refuses the whole
  proposal, rejection, a question without edits, auto_approve, task in a notes project, an unknown scope
  before any model call, the chat's notes commands; translations.
- Checked under Xvfb with a proposal and a chat answer prepared as the assistant writes them (no API key
  here): the chat of notes, the buttons under the answer, the proposals view with its diff, Approve
  writing the edits, mentions in the inspector and Link. `karkas notes` without a key ends with
  "the assistant failed: ANTHROPIC_API_KEY is not set". Not verified here: a real model run (needs a key).
