// Ported from tests/sema/null_to_nonnullable_test.cpp
//   (BEZEL_TEST null_to_nonnullable, unbox_and_widen_ok).
//
// ACCEPT: the Object unbox and numeric widening are unaffected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box public value: int := 0; public constructor(v: int) value := v; end constructor; end Box; class C public static method Main(): int var o: Object := 5; var oq: Object? := null; var lng: long := 7; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
