// Ported from tests/sema/generic_type_args_all_or_nothing_1687_test.cpp
//   (BEZEL_TEST generic_type_args_1687, record_ctor_full_explicit_accepted).
//
// Positive control: `new Pair<int, string>(1, "x")` — the spec's own
// suggested fix for q19 — is accepted (a full explicit list on the
// constructor call).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Pair<TFirst, TSecond>(First: TFirst, Second: TSecond); class C public static method Main(): int var p := new Pair<int, string>(1, "x"); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
