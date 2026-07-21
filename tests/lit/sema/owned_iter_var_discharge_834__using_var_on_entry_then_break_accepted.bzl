// Ported from tests/sema/owned_iter_var_discharge_834_test.cpp
//   (BEZEL_TEST owned_iter_var_discharge_834, using_var_on_entry_then_break_accepted).
//
// §29.2 / #834: the spec's canonical "discharge on entry, then break" pattern —
// `using var c := x;` immediately on entry converts the per-edge obligation
// into scope-bound disposal that dominates every exit. Must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box public constructor() end constructor; destructor end destructor; end Box; class C public static method Gen(): IOwningEnumerable<Box> yield return new Box(); end Gen; public static method Run(c: bool): int for var x in Gen() do using var _ := owned x; break; end; return 0; end Run; public static method Main(): int return Run(true); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
