# Bezel for Visual Studio Code

Syntax highlighting, editing rules and snippets for [Bezel](https://github.com/s8lap/bezel-lang), a compiled language with ownership-based memory management and no garbage collector.

This first version is editor support only: highlighting, bracket and comment handling, indentation around `… end;` blocks, and snippets. Diagnostics, go-to-definition and hover come with the language server planned for the compiler (`bezelc`).

## Features

- Highlighting for keywords, types, literals (including interpolated `$"…{x}…"` and verbatim `@"…"` strings), comments and `/// ` documentation comments, declarations and `end Name;` labels.
- Contextual keywords (`value`, `notnull`, `sendable`, `hashable`, `get`, `set`, `pure`, `scoped`, `old`) are coloured only in the positions where they are keywords, as the spec defines them; elsewhere they are ordinary identifiers.
- Highlighting inside Markdown: ` ```bezel ` (or ` ```bzl `) code blocks in `.md` files are coloured as Bezel in the editor. The Markdown preview pane uses its own highlighter and is not affected.
- Snippets: `module`, `class`, `record`, `method`, `if`, `for`, `while`, `try`, `tryfinally`, `usingvar`.

## `.bzl` and Bazel

Bezel source files use `.bzl`, the same extension as Bazel's Starlark files. A Bezel file starts with `module Name;`, and the extension declares that first line so VS Code can tell the two apart. If you also have a Bazel extension installed, set the language per workspace:

```json
"files.associations": { "*.bzl": "bezel" }
```

## How the grammar is made

The grammar is generated from the language specification: `scripts/gen_editor_grammars.py` reads the reserved and contextual keywords from the spec's Appendix B and writes `syntaxes/bezel.tmLanguage.json` (and the highlight.js language the documentation site uses). `scripts/check_docs.sh` fails when the generated files fall out of step with the spec. Do not edit the grammar by hand; change the generator.

`syntaxes/bezel.markdown.tmLanguage.json` is the small hand-written injection grammar for Markdown code blocks. It holds no keywords of its own: it marks the inside of a ` ```bezel ` fence as embedded Bezel and hands it to the generated grammar.

## Development

```sh
# from editors/vscode
npm install --no-save vscode-textmate@9 vscode-oniguruma@2
node test/tokenize.js          # tokenizes every bezel block in docs/ and every .bzl test program
MARKDOWN_GRAMMAR=<path to VS Code's markdown.tmLanguage> node test/markdown.js
                               # checks the injection marks exactly the bezel fences in docs/ as Bezel
node tools/readme-example.js   # re-renders the repository README's example as docs/images/readme-example-*.svg
                               # (GitHub cannot highlight Bezel; --check reports stale images)
npx @vscode/vsce package       # builds bezel-lang-<version>.vsix
code --install-extension bezel-lang-0.2.0.vsix
```

Publishing goes to the VS Code Marketplace (`vsce publish`) and Open VSX (`npx ovsx publish`), which serves VSCodium, Cursor and other VS Code-based editors. Both need a publisher account; the `publisher` field in `package.json` must match it.

## License

Apache 2.0, as the rest of the repository.
