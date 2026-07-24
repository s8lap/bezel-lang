// §22.8 (v0.226) single-assignment of readonly fields (#2187).
//
// `_f++` is a read-modify-WRITE. After an initial `:=` it is a second write to
// the readonly field — rejected by the single-assignment rule, same as `+=`.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _f: int; public constructor() _f := 1; _f++; end constructor; destructor end destructor; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)
