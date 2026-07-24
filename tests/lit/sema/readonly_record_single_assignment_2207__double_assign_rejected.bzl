// §22.8 (v0.226) single-assignment of readonly fields on RECORDS (#2207).
//
// §22.8: "A field declared in a class or record may include an initializer"
// — the single-assignment rule §7.4/#2116 narrowing depends on is
// type-agnostic. The #2187 fix enforced only the class side (the
// `inClass = enclosing->kind == Class` gate in DefiniteAssignment.cpp routed
// records to the plain da.run path with roCtorMode_ false), so a record's
// authored ctor could double-assign a readonly field — and §7.4 narrowing
// applies to record fields too, so the #2187 SEGV (stored-lambda derefing a
// value the ctor later nulled) reproduced on records. Reject the second
// assignment.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; record R public readonly _f: int; public constructor(x: int) _f := x; _f := 0; end constructor; public static method Main(): int return 0; end Main; end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)