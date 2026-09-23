// fix #2547 (codegen, LlvmBackend_Exprs.cpp CG8's MemberAccessExpr fall-through,
// v0.236; sibling of #2477's lowerCall/lowerNew diagnostics): the THIRD
// #605 / generic-new(T) silent-zero shape the original issue cataloged but
// the #2477 fix did not cover. The CG8 path handles a *bare* MemberAccessExpr
// read `obj.f` — NOT a call (lowerCall's `if (!ms)` is for the paren form),
// not a `new` (lowerNew is for the construction form). When the receiver is
// a concrete type (Object/primitive) and the member is unknown to the type
// system, both fieldAddr and staticFieldGlobal return null and CG8 falls
// through to `return nullptr` — so the only prior signal was the generic
// #2434 internal warning emitted by lowerLocalDecl's zero-init fallback,
// which named the slot but not the member or the receiver type, leaving the
// slot-degraded-to-zero case silently wrong.
//
// This control pins the new SPECIFIC diagnostic that names the member and
// the receiver type for that CG8 shape. The program exercises BOTH the
// bare-MemberAccessExpr-read path (this file) AND the existing call-via-
// MemberAccessExpr path (sibling `tests/lit/no_member_unresolvable_warns_
// 2477.bzl`) on the same source-text shape — the difference is parens. Both
// diagnostics reference spec §3 / issue #605 in their message text; the new
// CG8 fall-through diagnostic cites #2547. The existing #2434 generic
// warning is unchanged (the sibling control test
// `tests/lit/local_decl_zero_init_fallback_warns_2434.bzl` continues to pin
// that text).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main(): int var o: Object := new Object(); var s := o.Anything; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: codegen cannot read member 'Anything' on receiver of type 'Object': permissive acceptance (spec §3 / issue #605) admits this site but no concrete member dispatch exists (CG8's fall-through degrades the slot to zero via lowerLocalDecl's initializer-failed fallback — #2547)
