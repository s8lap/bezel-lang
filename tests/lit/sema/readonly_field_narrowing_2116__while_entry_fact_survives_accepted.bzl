// §7.4 (v0.214) / #2118 / #2116: a readonly-field fact that HOLDS AT LOOP
// ENTRY survives the loop — an ambient-rooted fact is never invalidated (no
// writer, §7.4 v0.214), so neither the body's intervening call nor the loop
// join drops it; both the in-body use and the after-loop use are ACCEPTED.
// No diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Noise() end Noise; public method Len(): int if _name <> null then var i := 0; while i < 3 do Noise(); i := i + _name.Length; end; return _name.Length; end; return 0; end Len; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
