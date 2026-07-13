// Ported from tests/sema/nullable_field_narrowing_862_test.cpp
//   (BEZEL_TEST nullable_field_narrowing_862, copy_to_local_then_guard_accepted).
//
// §7.4 (safe pattern): copying the nullable field to a local and narrowing the
// local is the correct idiom — it must be ACCEPTED with no errors.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Container public _name: string?; public constructor(n: string) _name := n; end constructor; destructor end destructor; end Container; class C public static method Main(): int using var c := new Container("hello"); var n := c._name; if n <> null then return n.Length; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
