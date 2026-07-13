// Ported from tests/sema/generic_method_constraint_callsite_746_test.cpp
//   (BEZEL_TEST method_constraint_callsite_746, int_satisfies_value).
//
// §14.2: an `int` (a value type) SATISFIES `where T: value` — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class F public static method Id<T>(v: T): T where T: value return v; end Id; end F; class C public static method Main(): int var r := F.Id<int>(42); return r; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
