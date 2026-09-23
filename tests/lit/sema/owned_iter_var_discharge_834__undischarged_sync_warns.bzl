// Ported from tests/sema/owned_iter_var_discharge_834_test.cpp
//   (BEZEL_TEST owned_iter_var_discharge_834, undischarged_sync_warns).
//
// §29.2 / §28.3 v0.157 / #1329 / #834 (headline): the owned iteration variable
// not discharged in the body — auto-dropped per-iteration with a must-use
// WARNING (no hard error; that was the pre-v0.157 behaviour). Before #834 this
// was silently accepted (no warning, a silent leak). Before #1329 it was a hard
// error. Now it is a must-use warning: no errors, one auto-drop warning.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Box public constructor() end constructor; destructor end destructor; end Box; class C public static method Gen(): IOwningEnumerable<Box> yield return new Box(); end Gen; public static method Run(c: bool): int for var x in Gen() do end; return 0; end Run; public static method Main(): int return Run(true); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `x` is implicitly disposed at the end of each loop iteration
// CHECK-SAME: (§29.2/§28.3 auto-drop)
