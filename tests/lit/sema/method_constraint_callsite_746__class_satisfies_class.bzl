// Ported from tests/sema/generic_method_constraint_callsite_746_test.cpp
//   (BEZEL_TEST method_constraint_callsite_746, class_satisfies_class).
//
// §14.2: a class type argument SATISFIES `where T: class` — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Widget public n: int := 0; end Widget; static class F public static method Id<T>(v: T): T where T: class return v; end Id; end F; class C public static method Main(): int using var w := new Widget(); var r := F.Id(w); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
