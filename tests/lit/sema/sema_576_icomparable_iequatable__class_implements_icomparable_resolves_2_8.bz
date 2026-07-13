// Ported from tests/sema/icomparable_iequatable_576_test.cpp
//   (BEZEL_TEST sema_576_icomparable_iequatable, class_implements_icomparable_resolves_2_8).
//
// §2.8 / §2.3: A class implementing IComparable<Self> with CompareTo must
// resolve the base-type name without error.  This is the canonical repro from
// issue #576.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Test; class MyInt : IComparable<MyInt> private _val: int; public constructor(v: int) _val := v; end constructor; public method CompareTo(other: MyInt): int if _val < other._val then return -1; end; if _val > other._val then return 1; end; return 0; end CompareTo; end MyInt;

// NODIAG-NOT: error
// NODIAG-NOT: §
