#pragma once
// Markdown in the note editor (ADR 0040): a language definition of ImGuiColorTextEdit whose tokenizer
// colors headings, list markers and task boxes, quotes, rules, fences, inline code, links, [[links]],
// bold, italic and strikethrough, line by line (the editor colors lines alone, so a fenced block's inner
// lines stay plain text).
#include <cstdint>  // TextEditor.h uses uint8_t
#include <TextEditor.h>

namespace kk::gui {

const TextEditor::LanguageDefinition& markdown_language();
// The palette entries the Markdown tokens use, on top of `base`.
TextEditor::Palette markdown_palette(TextEditor::Palette base);

}  // namespace kk::gui
