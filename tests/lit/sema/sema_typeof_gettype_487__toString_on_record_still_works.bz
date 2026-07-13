// Ported from tests/sema/typeof_gettype_487_test.cpp
//   (BEZEL_TEST sema_typeof_gettype_487, toString_on_record_still_works).
//
// The universal `ToString` on a record still resolves cleanly (no regression
// from the GetType typing addition to the universal-member dispatch).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(x: int, y: int); class C public method M(p: Point): string return p.ToString(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
