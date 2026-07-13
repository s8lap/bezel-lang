// Ported from tests/sema/generic_type_args_all_or_nothing_1687_test.cpp
//   (BEZEL_TEST generic_type_args_1687, record_ctor_target_typed_shorthand_accepted).
//
// Positive control: the §17.7 new() shorthand with a typed local target
// resolves the class-level type arguments from the DECLARED TYPE, not from
// the constructor arguments — this is not "inference from constructor
// arguments", so §14.5's Scope paragraph does not forbid it.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Pair<TFirst, TSecond>(First: TFirst, Second: TSecond); class C public static method Main(): int var p: Pair<int, string> := new(1, "x"); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
