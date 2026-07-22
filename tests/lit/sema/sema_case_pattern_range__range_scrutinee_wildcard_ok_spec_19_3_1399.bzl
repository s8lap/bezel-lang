// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, range_scrutinee_wildcard_ok_spec_19_3_1399).
//
// §19.3 (#1399): a `_` (wildcard) pattern over a Range scrutinee is valid —
// wildcard matches any value without comparing. Pin that it is NOT rejected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var lo := 1; var hi := 3; return (case (lo..hi) of _ => 0); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
