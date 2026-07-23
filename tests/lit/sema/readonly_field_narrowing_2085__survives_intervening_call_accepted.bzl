// §7.4 (v0.214, proposal 0058, #2085): a `this`-rooted readonly-field fact is
// NEVER invalidated by an intervening call (a readonly field has no writer on
// any path). The `<> null` fact survives the `Noise()` call between the guard
// and the use — unlike a mutable field, which would require a local copy.
// ACCEPT with no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Noise() end Noise; public method Len(): int if _name <> null then Noise(); return _name.Length; end; return 0; end Len; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
