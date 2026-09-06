// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Was: sema_unknown_member__interface_typed_receiver_permissive.bzl (ported
// from tests/sema/unknown_member_test.cpp,
// BEZEL_TEST sema_unknown_member, interface_typed_receiver_permissive).
//
// fix #3953 (§11.2/§11.3, v0.294): a plain interface-typed receiver is now
// treated as known-complete, like class/record/enum/choice — `lookupMember`
// already BFS-walks an interface's own `members` and its extended
// interfaces' `baseTypes` uniformly with every other type kind, so a member
// name that resolves to nothing here genuinely does not exist on the
// interface. Before this fix this exact program compiled with NO diagnostic
// and codegen's "permissive acceptance" backstop silently zero-filled the
// call's result at runtime (measured: `s.SomethingNotDeclared()` returned 0,
// exit 0, no diagnostic at any severity) — this file used to pin that
// silence as the correct behavior. It no longer is: see
// unknown_member_interface_3953_test.cpp for the full positive/negative grid
// this fix ships (typo member, member declared only on the implementing class,
// member declared only on a DIFFERENT interface the class also implements,
// property/method/field-shaped access) and the two verified-working
// remedies (narrow to the concrete type via `is`/`as`, or declare the member
// on the interface itself).

module M; interface IShape method Area(): int; end IShape; class C public method M(s: IShape): int => s.SomethingNotDeclared(); end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `SomethingNotDeclared` on interface `IShape`
// CHECK-SAME: (§11.2/§11.3)
