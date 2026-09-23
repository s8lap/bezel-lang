// Ported from tests/sema/owned_iter_var_discharge_834_test.cpp
//   (BEZEL_TEST owned_iter_var_discharge_834, undischarged_break_warns).
//
// §29.2 / §28.3 v0.157 / #1329 / #834: `break` without discharging `x` on
// the break path — `x` is still-owned when breaking. Must emit the auto-drop
// WARNING (emitLoopDisposals fires the iter-var scope on break). Not an error.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Box public constructor() end constructor; destructor end destructor; end Box; class C public static method Gen(): IOwningEnumerable<Box> yield return new Box(); end Gen; public static method Run(c: bool): int for var x in Gen() do if c then break; end; x.Dispose(); end; return 0; end Run; public static method Main(): int return Run(true); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `x` is implicitly disposed at the end of each loop iteration
// CHECK-SAME: (§29.2/§28.3 auto-drop)
