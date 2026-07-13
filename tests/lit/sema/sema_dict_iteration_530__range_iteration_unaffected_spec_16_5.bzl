// Ported from tests/sema/dict_iteration_diag_530_test.cpp
//   (BEZEL_TEST sema_dict_iteration_530, range_iteration_unaffected_spec_16_5).
//
// §16.5 / §29.2: `for var i in 0..5` — an integer range literal.
// Ranges use a completely separate inference path (RangeExpr); the guard
// must not interfere.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class V end V; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, V>, arr: Array<int>, s: Set<int>, n: int) for var i in 0..5 do n := n + i; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
