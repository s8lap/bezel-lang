// §7.4 (v0.214, proposal 0058, #2085) composed with §22.7: the construction-
// window exclusion is TEXTUAL (the constructor body only). A non-virtual helper
// reached during construction is elsewhere, so it narrows `this._name` normally.
// During construction the narrowing observes either the null watermark (the
// guard is false) or the field's single assigned value (readonly, final by
// §22.8) — the §22.7 residual, inherited not enlarged. ACCEPT with no
// diagnostics.
//
// fix #6555 (§21.9, v0.338) flipped the second control here: the constructor
// body's `Helper();` discards `Helper`'s `int` result, which §21.9's base
// unused-result rule now warns on. The row's SUBJECT — §7.4's construction-
// window exclusion and the §22.7 narrowing residual it inherits — is untouched
// by the flip: the program is still ACCEPTED, and `NODIAG-NOT: error` (the
// acceptance) stays. The old blanket `NODIAG-NOT: §` is replaced by a W21_9
// check pinning the warning §21.9 now requires — a strengthening (a specific
// diagnostic pinned where silence used to be), not a relaxation. Reading it as
// a regression signal is precisely wrong: §21.9 requires this diagnostic.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=W21_9 --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; Helper(); end constructor; destructor end destructor; public method Helper(): int if _name <> null then return _name.Length; end; return 0; end Helper; end C;

// NODIAG-NOT: error
// W21_9: warning: the result of this expression statement is unused
