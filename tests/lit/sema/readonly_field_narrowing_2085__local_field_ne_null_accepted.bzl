// §7.4 (v0.214, proposal 0058, #2085): `local._f` narrows — one readonly field
// off a narrowing-tracked local root. The fact rides the root; here the root is
// a borrowed parameter that is never reassigned, so the narrowing holds.
// ACCEPT with no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Holder public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; end Holder; class C public static method Use(h: Holder): int if h._name <> null then return h._name.Length; end; return -1; end Use; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
