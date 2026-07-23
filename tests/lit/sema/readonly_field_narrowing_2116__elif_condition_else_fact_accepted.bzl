// §7.4 (v0.214) / #2116: the sound converse of the elif-staleness reject —
// an elif condition DOES see the false-branch facts of the preceding
// conditions. `_name = null` was false when the elif condition runs, so
// `_name` is known non-null there and `_name.Length > 0` is ACCEPTED, as is
// the elif BODY's use. No diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Len(): int if _name = null then return 0; elif _name.Length > 0 then return _name.Length; end; return 9; end Len; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
