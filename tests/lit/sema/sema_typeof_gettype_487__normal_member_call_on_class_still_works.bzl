// Ported from tests/sema/typeof_gettype_487_test.cpp
//   (BEZEL_TEST sema_typeof_gettype_487, normal_member_call_on_class_still_works).
//
// A normal method call on a class still resolves cleanly.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Calc public method Add(a: int, b: int): int => a + b; end Calc; class C public method M(c: Calc): int return c.Add(1, 2); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
