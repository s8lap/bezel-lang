// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, discard_slot_ok_spec_27_4).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Entry(K: int, V: int); class C public static method Main(): int var f: (Entry) -> int := lambda(Entry(_, v)) => v; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
