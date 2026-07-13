// Ported from tests/sema/generic_inconsistent_type_arg_896_test.cpp
//   (BEZEL_TEST generic_inconsistent_type_arg_896, inline_out_var_not_inconsistent_1164).
//
// §22.4 / §14.1 / #1164: a generic method `G<T>(a: T, out o: T)` called with an
// inline `out var s` must NOT be flagged as inconsistent inference. The inline
// `out var` has no type of its own — its type is INFERRED FROM the matched out
// parameter (§22.4), so it must not contribute an independent binding to T
// (which would clash with T=int from the value argument `5`). ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class U public static method G<T>(a: T, out o: T): T where T: value o := a; return a; end G; public static method Main(): int var r := U.G<int>(5, out var s); return r + s; end Main; end U;

// NODIAG-NOT: error
// NODIAG-NOT: §
