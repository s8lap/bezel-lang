// fix #2477 (codegen, LlvmBackend_Exprs2.cpp lowerNew, v0.236): the second
// diagnostic the structural fix adds — the abstract-generic-body `new T()`
// shape. Per #2477, the dead abstract template body (emitted for generic
// methods that have NO call-site monomorphization) emits a `new T()`
// NewExpr whose construction has no concrete class/record to dispatch to
// (`genericSubstSemaTy_` is empty there). The codegen falls through every
// construction branch and lowerNew returns nullptr; the slot is never
// initialized and the existing #2434 generic warning fires from
// lowerLocalDecl's zero-init fallback. #2477's fix adds a SPECIFIC
// diagnostic at this fall-through that names the type parameter and
// references the abstract-body shape — so the silence→zero case is
// observable in source. The existing #2434 generic warning is unchanged
// (the sibling control test `tests/lit/local_decl_zero_init_fallback_warns
// _2434.bzl` continues to pin that text).
//
// This control pins the new diagnostic for the abstract-template-body
// `new T()` shape. The first half of the fix (the Object-receiver and
// bare-T-call fall-through diagnostic) is pinned by the sibling
// `tests/lit/no_member_unresolvable_warns_2477.bzl`.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Widget public constructor() end constructor; destructor end destructor; end Widget; static class Factory public static method Make<T>(): int where T: class, new() using var t := new T(); return 1; end Make; end Factory; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: codegen cannot construct 'new T()' in the dead abstract template body: no monomorphization binding exists for this type parameter (spec #14.1 / #14.2 permits the form but the abstract body is never invoked — real calls route through the monomorphized clone — #2477)
