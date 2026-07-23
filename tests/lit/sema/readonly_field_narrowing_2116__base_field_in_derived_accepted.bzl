// §7.4 (v0.214) / #2116: an INHERITED nullable readonly field narrows in a
// derived-class method exactly as an own field — the bare `_name` and the
// explicit `base._name` spellings resolve to the same (base-declared)
// FieldSymbol, so both take the ambient root and share one fact. ACCEPT with
// no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class B public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; end B; class D : B public constructor(n: string?) base(n); end constructor; destructor end destructor; public method Len(): int if _name <> null then return _name.Length; end; return 0; end Len; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
