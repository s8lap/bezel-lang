// §22.8 (v0.226) single-assignment of readonly fields (#2187).
//
// A readonly field assigned inside a loop body could be assigned on more than
// one iteration — a potential re-assignment the compiler cannot rule out (§7.4
// narrowing is carved out of the construction window, so no per-field flow
// fact bounds the loop). §22.8's single-assignment rule rejects it (the
// conservative, sound reading).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _f: int; public constructor() for var i in 0..3 do _f := i; end; end constructor; destructor end destructor; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` inside a loop
// CHECK-SAME: (§22.8 single-assignment)
