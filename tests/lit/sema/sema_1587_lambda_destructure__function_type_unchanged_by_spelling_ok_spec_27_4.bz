// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, function_type_unchanged_by_spelling_ok_spec_27_4).
//
// §27.4: "the lambda's function type is unchanged by the spelling
// (`(Entry) -> R`)" — a destructuring lambda must be assignable to an
// ordinary `(Entry) -> int` typed local (checked positively above) AND must
// be usable as a call argument expecting that same function type.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Entry(K: int, V: int); class C public static method Apply(e: Entry, f: (Entry) -> int): int return f(e); end Apply; public static method Main(): int return Apply(new Entry(1, 2), (Entry(k, v)) => k + v); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
