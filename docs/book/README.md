# The Bezel Language

Bezel is a compiled language for services and tools, with ownership-based memory management and no garbage collector. The compiler tracks ownership of every heap object and guarantees each is disposed exactly once; borrowing lets references be passed without transferring ownership, with lifetimes checked at compile time.

This site is generated from the documents in the repository's `docs/` folder at every spec revision. The documents themselves are the source; the site splits them into pages, adds search and navigation, and changes no text.

## Where to start

- **Guides** — short, example-first introductions: start with [Memory and Ownership](guide-01/index.md), then [Errors and Nullability](guide-02/index.md).
- **Reference** — the normative [Language Specification](spec/index.md) and [Standard Library](stdlib/index.md), and the dense [Writing Reference](writing/index.md) for writing Bezel code.
- **Background** — the [Design Rationale](rationale/index.md), which records why each decision was made and which alternatives were declined, and the [Changelog](changelog/index.md), one entry per revision.

The specification is normative: where it and any other page disagree, the specification wins.
