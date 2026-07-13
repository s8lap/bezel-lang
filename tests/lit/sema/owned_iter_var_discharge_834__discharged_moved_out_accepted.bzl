// Ported from tests/sema/owned_iter_var_discharge_834_test.cpp
//   (BEZEL_TEST owned_iter_var_discharge_834, discharged_moved_out_accepted).
//
// §29.2 / #834: discharge by moving the iteration variable into an owned sink
// (`using var y := move x;`) — the move discharges `x` (it becomes
// moved-from). Must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box public constructor() end constructor; destructor end destructor; end Box; class C public static method Gen(): IOwningEnumerable<Box> yield return new Box(); end Gen; public static method Run(c: bool): int for var x in Gen() do using var y := move x; end; return 0; end Run; public static method Main(): int return Run(true); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
