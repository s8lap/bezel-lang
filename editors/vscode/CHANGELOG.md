# Changelog

## Unreleased

- The contextual keyword `of` is highlighted where it closes a `case` head on the same line (`case x of`, `(case f(y) of`); an `of` anywhere else stays an identifier (spec Appendix B.2).

## 0.2.0

- Bezel code blocks in Markdown files (` ```bezel ` and ` ```bzl ` fences) are highlighted as Bezel, through an injection grammar into VS Code's Markdown grammar.

## 0.1.0

- First version: TextMate grammar generated from the spec's Appendix A and B, language configuration, snippets.
