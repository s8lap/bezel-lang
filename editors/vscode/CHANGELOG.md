# Changelog

## Unreleased

- Every contextual keyword of spec Appendix B.2 is highlighted in its position, in both the TextMate grammar and the docs site's highlight.js language, and stays an identifier elsewhere: `of` closing a `case` head, on the same line or a later one (`case x of`, `(case f(y) of`); `step` after a range (`0..100 step 2`); `convention` in an `extern "lib"` header, and the `env` user-data marker and `variadic(N)` inside the block; the `:= symbol "name"` alias clause; `serialized skip`, `serialized "name" optional` and `equality skip`; the class-body `polymorphic ("tag": Type, …);` member; `operator` after `static method`, after an interface's `static` and in `end operator +;`; the call-site `scoped` marker (`new Reader(scoped s)`); `pure` before `constructor`, `readonly` and the virtual kinds; `set(owned)` accessors and the `private set` auto-property shorthand anywhere on a line; every constraint in a `where T: sendable, notnull` list, not just the first. `old(` is now a keyword only inside an `ensures (…)` clause. `value` as a setter's implicit parameter and `result` in an `ensures` clause stay plain (their reading depends on the enclosing declaration).
- The operator name after `method operator` is the operator token alone (it used to run on into the parameter list), and an accessor's leading visibility (`private set => …`) is coloured as a modifier.
- New tests: `test/contextual.js` and `test/hljs.js` check the contextual positions against shared fixtures, and `test/hljs.js` also highlights every Bezel block in the docs with highlight.js 10.1.1, the version the docs site uses.

## 0.2.0

- Bezel code blocks in Markdown files (` ```bezel ` and ` ```bzl ` fences) are highlighted as Bezel, through an injection grammar into VS Code's Markdown grammar.

## 0.1.0

- First version: TextMate grammar generated from the spec's Appendix A and B, language configuration, snippets.
