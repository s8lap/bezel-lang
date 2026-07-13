// Ported from tests/sema/owned_iter_var_discharge_834_test.cpp
//   (BEZEL_TEST owned_iter_var_discharge_834, conditional_all_paths_discharged_accepted).
//
// §29.2 / #834: discharge on ALL paths of a conditional — both branches
// dispose `x`. Must be ACCEPTED (an over-reject would wrongly flag this).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box public constructor() end constructor; destructor end destructor; end Box; class C public static method Gen(): IOwningEnumerable<Box> yield return new Box(); end Gen; public static method Run(c: bool): int for var x in Gen() do if c then x.Dispose(); else x.Dispose(); end; end; return 0; end Run; public static method Main(): int return Run(true); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
