// §7.4 (v0.214, proposal 0058, #2085) composed with §22.7: the construction-
// window exclusion is TEXTUAL (the constructor body only). A non-virtual helper
// reached during construction is elsewhere, so it narrows `this._name` normally.
// During construction the narrowing observes either the null watermark (the
// guard is false) or the field's single assigned value (readonly, final by
// §22.8) — the §22.7 residual, inherited not enlarged. ACCEPT with no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; Helper(); end constructor; destructor end destructor; public method Helper(): int if _name <> null then return _name.Length; end; return 0; end Helper; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
