// §22.8 (v0.226) single-assignment of readonly fields (#2187).
//
// A readonly field with a FIELD INITIALIZER is assigned as step 2 of a
// base/implicit-base constructor, before the body (§22.8). Re-assigning it in
// the body is therefore a SECOND assignment — the same finality-breaking hole
// (probe-confirmed SEGV in `lam$0` via `_f := "abc"` initializer + `_f := null`
// body + this-escape). Reject the body assignment.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _f: string? := "abc"; public constructor() _f := null; end constructor; destructor end destructor; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)
