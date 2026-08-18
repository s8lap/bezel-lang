# Proposal 0063 — Exempt discarded borrow-of-receiver results from the §21.9 warning

| | |
|---|---|
| **Status** | graduated-v0.215 — superseded — graduated v0.215, sharpened: the landed rule adds two conditions the draft lacked — the result's static type must equal the receiver's (else same-source *queries* like `FirstOrNull()` would be silenced), and the exemption covers method-call results only (not accessor reads). Base-typed fluent returns stay conservatively warned. |
| **Origin** | 2026-07-21 writer-seat sweep: the single highest-*frequency* language-level friction found. `StringBuilder`'s fluent API returns `StringBuilder` from every `Append`/`AppendLine` (stdlib §3.1), so the canonical `sb.Append(x);` statement draws §21.9's discarded-result warning at every call site; the sanctioned remedies (`_ := sb.Append(x);` everywhere, or a unit-returning wrapper) are ceremony against the stdlib's own idiom. |
| **Written against** | spec v0.214 |

## The rule

A discarded expression-statement result that is a **borrow whose source set is exactly the statement's own receiver chain** (the §28.3 calls-lend classification — a `return this` fluent result is the canonical instance) is exempt from §21.9's discard warning. Rationale: discarding such a borrow loses nothing — the receiver is still held by the caller; the "value" was only ever the receiver back again. The warning's purpose (a *computed* result the programmer may have meant to use) does not apply. Everything else is unchanged: owned results keep the §17.13/§21.9 regime (hard error for non-trivial-dispose), non-receiver-sourced borrows keep the warning, and `_ :=` remains available.

## Why not the alternatives

Unit-returning stdlib siblings double the API surface; blanket-silencing trivial-dispose discards would also silence genuinely-forgotten computed values (`s.Trim();`). The receiver-borrow test is narrow, principled (it falls out of the K1.5 classification already computed at every site), and matches intent exactly.

## Cost

One §21.9 sentence + impl (the classification exists; the check is a lookup). A caller who *wanted* to be warned that a fluent API was mis-used as mutating (when it actually copies) is unprotected — but Bezel's fluent returns are `this`-borrows by §28.3's calls-lend default, so a copying fluent API would return owned and keep its protections.
