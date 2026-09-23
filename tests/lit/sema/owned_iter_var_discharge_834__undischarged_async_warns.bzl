// Ported from tests/sema/owned_iter_var_discharge_834_test.cpp
//   (BEZEL_TEST owned_iter_var_discharge_834, undischarged_async_warns).
//
// §29.2 / §28.3 v0.157 / #1329 / #834 (async form): `await for` over an
// IOwningAsyncEnumerable<Box> with the iteration variable not discharged — must
// emit the auto-drop WARNING (not an error, matching the sync form).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Box public constructor() end constructor; destructor end destructor; end Box; class C public static async method Gen(): IOwningAsyncEnumerable<Box> yield return new Box(); end Gen; public static async method Run(c: bool): Task await for var x in Gen() do end; end Run; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `x` is implicitly disposed at the end of each loop iteration
// CHECK-SAME: (§29.2/§28.3 must-use / auto-drop)
