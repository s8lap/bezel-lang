// fix #2434 (codegen, LlvmBackend_Setup.cpp Emitter::lowerLocalDecl, v0.235):
// negative control for the sibling ACCEPT-WITH-WARNING test (this dir,
// local_decl_zero_init_fallback_warns_2434.bzl). #2434's new internal
// Warning is gated on `init != nullptr` — a declaration-only local
// (`var x: int;`, no initializer expression at all) must stay COMPLETELY
// SILENT, exactly as it was before this fix: this is the common, legitimate
// case definite assignment (Appendix C.1) exists to make safe (the zero
// written here is never observed before a later assignment). A regression
// that widened the new Warning's condition to fire on EVERY zero-init
// (dropping the `init` check) would turn this clean-compile control red —
// pinning it here catches that shape of regression directly, the same way
// the sibling test pins the case that SHOULD warn.
//
// Covers the two slot shapes #2423's own commit message named explicitly:
// a plain scalar (`int`) and the `{i1, i64}` nullable-value-type struct
// layout (`int?`).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var a: int; var b: int?; a := 1; return a; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: §
