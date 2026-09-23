// fix #2477 (codegen, LlvmBackend_Calls2.cpp lowerCall's `if (!ms)` fall-through,
// v0.236; sibling in LlvmBackend_Exprs2.cpp lowerNew): the structural fix that
// turns the silent-zero codegen gaps the issue catalogs into OBSERVABLE
// diagnostics. Per #2477, two distinct shapes had no codegen at all (codegen
// fell through to nullptr from the §3 / #605 permissive "unknown member" call
// path, and from the `new T()` body of a generic method genericSubstSemaTy_
// cannot resolve) — the only signal was the generic #2434 internal warning
// emitted by lowerLocalDecl's zero-init fallback, which named the slot but not
// the member or the receiver type. #2477's structural fix is to add a SPECIFIC
// diagnostic at the codegen fall-through that identifies the member name and
// receiver type — so the silence→zero case is observable in source. The
// existing #2434 generic warning is unchanged (the sibling control test
// `tests/lit/local_decl_zero_init_fallback_warns_2434.bzl` continues to pin
// that text).
//
// This control pins the new diagnostic for the Object-typed receiver shape
// (spec §3 / #605's "unknown member" permissive acceptance on a concrete
// receiver). The same diagnostic is emitted for the bare-T in the dead
// template body and for the `new T()` abstract-template-body shape via
// separate code paths.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public method M(o: Object): int var s := o.Anything(); return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: codegen cannot dispatch member 'Anything' on receiver of type 'Object': permissive acceptance (spec #3 / issue #605) admits this site but no concrete member dispatch exists (lowerLocalDecl's zero-init fallback will degrade the slot — #2477)
