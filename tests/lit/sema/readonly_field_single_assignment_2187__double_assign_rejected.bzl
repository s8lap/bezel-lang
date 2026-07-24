// §22.8 (v0.226) single-assignment of readonly fields (#2187).
//
// §22.8: a `readonly` field is assigned "exactly once" — the single-assignment
// rule §7.4/§21.1 rely on ("member-level runtime single-assignment"; "the
// field's single assigned value, final by §22.8"). Assigning `_f` twice in the
// constructor body was ACCEPTED (only the PLACEMENT was enforced, not the
// once-ness), which is exactly what broke the #2116 readonly-field narrowing:
// a second assignment makes an observed non-null value NOT final. Reject the
// second assignment.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _f: string?; public constructor() _f := "a"; _f := null; end constructor; destructor end destructor; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)
