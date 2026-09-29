// fix #5032 (codegen, LlvmBackend_Exprs.cpp, v0.307): inside a generic
// method's DEAD unspecialized template body (issue #490 — every generic
// method is also emitted once with its type parameter erased to a
// non-pointer placeholder, and that body is never invoked; only the
// per-call-site SPECIALIZED clone runs), a string-interpolation hole whose
// receiver is the erased type parameter (`$"{v.Area()}xy"` where `v: T`)
// used to hit an UNCONDITIONAL #1386 Error, rejecting the whole program
// even though the real, called body never observes the failure. The fix
// gives this site the same three-outcome dispatch #2434/#2491 already use:
// this is the "everything else" (c) branch — degrade to a placeholder value
// and report only a Warning, per issue #2924's "four-way-mirror" discipline.
//
// This control pins the new diagnostic text. The gtest sibling
// (tests/codegen/generic_template_tparam_member_5032_test.cpp) owns the
// runtime VALUES on stdout for both this shape and the case-arm sibling fix
// (LlvmBackend_Exprs2.cpp, pinned by the companion
// tests/lit/case_arm_t_member_dead_template_body_warns_5032.bzl); `runAot`
// cannot see diagnostic text on a successful compile (records diagnostics
// only on a compile REJECT), so this file owns the TEXT.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module InterpDead5032; interface IShapeN method Area(): int; end IShapeN; static class GN public static method Interp<T>(v: T): string where T: IShapeN return $"{v.Area()}xy"; end Interp; end GN; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: lowerExpr returned null for interpolated-string expression part (degraded to a placeholder rendering) — #1386
