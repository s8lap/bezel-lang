// §22.8 (v0.226) single-assignment of readonly fields (#2187).
//
// A compound assignment (`+=`) to a readonly field already assigned by an
// earlier `:=` is a SECOND write — single-assignment covers every assignment
// operator, not just `:=`. Reject it.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _f: int; public constructor() _f := 1; _f += 2; end constructor; destructor end destructor; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)
