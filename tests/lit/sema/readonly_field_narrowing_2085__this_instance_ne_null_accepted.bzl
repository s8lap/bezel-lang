// §7.4 (v0.214, proposal 0058, #2085): a nullable `readonly` INSTANCE field
// narrows off `this` — bare `_f` (implicit this) and explicit `this._f` — after
// a `<> null` guard. A readonly field has no writer after construction (§22.8),
// so the mutable-field TOCTOU refusal does not apply. ACCEPT with no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Len(): int if _name <> null then return _name.Length; end; return 0; end Len; public method Len2(): int if this._name <> null then return this._name.Length; end; return 0; end Len2; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
