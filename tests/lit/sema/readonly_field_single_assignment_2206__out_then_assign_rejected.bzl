// §22.8 (v0.226) single-assignment of readonly fields (#2206, route A).
//
// Order-independence guard: the `out` write seeds `readonlyMaybeAssigned_` on
// the FIRST write, so a later direct `:=` of the same readonly field is the
// second assignment and is rejected — regardless of which route wrote first.
// This pins that the `out` operand genuinely enters the once-set, not just a
// diagnostic on the `out` itself.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _f: string?; public constructor() SetIt(out _f); _f := "b"; end constructor; destructor end destructor; public method SetIt(out x: string?) x := "a"; end SetIt; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)
