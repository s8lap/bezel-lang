// §22.8 (v0.226) single-assignment of readonly fields (#2206, route A).
//
// An `out` argument whose operand is a readonly field, textually inside a loop
// body, could write the field on more than one iteration — the same shape as
// #2187's direct-assign-in-loop reject. The fix routes the `out` operand
// through checkReadonlyOnceness, which gates on `inLoopDepth_` exactly as the
// AssignmentStmt path does.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _f: int; public constructor() for var i in 0..3 do SetIt(out _f); end; end constructor; destructor end destructor; public method SetIt(out x: int) x := 0; end SetIt; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` inside a loop
// CHECK-SAME: (§22.8 single-assignment)
