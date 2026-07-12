// Ported from tests/sema/null_to_nonnullable_test.cpp
//   (BEZEL_TEST null_to_nonnullable, nullable_targets_ok).
//
// ACCEPT: null into nullable targets (value, reference, string).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box public value: int := 0; public constructor(v: int) value := v; end constructor; end Box; class C public static method Main(): int var a: int? := null; var b: Box? := null; var c: string? := null; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
