// fix #5032 (codegen, LlvmBackend_Exprs2.cpp lowerCaseExpr, v0.307): inside a
// generic method's DEAD unspecialized template body (issue #490 — see the
// companion tests/lit/interp_t_member_dead_template_body_warns_5032.bzl for
// the full mechanism), a `case` type-pattern arm over a T-typed scrutinee
// (`case v of var q: SqN => q.Area() + 10, _ => 1`) has its pattern
// statically never match there (T is erased to a non-pointer placeholder),
// but the arm BODY is still lowered unconditionally to keep the template's
// IR complete, and `q.Area()` hits the identical #484 non-pointer-receiver
// guard as the interpolation sibling. Before this fix that fell straight
// through to an untermined-block IR-verifier abort with NO diagnostic at
// all — worse than the interpolation site's unconditional Error, because
// there was not even the #1386 ICE text. The fix synthesizes a placeholder
// value in both the Error and Warning branches so every block still gets a
// terminator, and reports only a Warning for this "everything else" (c)
// branch, mirroring the LAMBDA-body precedent for this exact shape (this
// file's own #1255 fix) and the sibling #2434/#2491 three-outcome dispatch.
//
// This control pins the new diagnostic text. The gtest sibling
// (tests/codegen/generic_template_tparam_member_5032_test.cpp) owns the
// runtime VALUES on stdout for both this shape and the interpolation
// sibling fix; `runAot` cannot see diagnostic text on a successful compile,
// so this file owns the TEXT.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module CaseDead5032; interface IShapeN method Area(): int; end IShapeN; class SqN: IShapeN public constructor() end constructor; public method Area(): int return 7; end Area; end SqN; static class GN public static method CaseT<T>(v: T): int where T: IShapeN return (case v of var q: SqN => q.Area() + 10, _ => 1); end CaseT; end GN; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: case-arm expression body failed to lower to a value (degraded to a zero/null arm result) — #1386
