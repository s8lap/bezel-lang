// fix #5012, #5013 (spec §2.7 "Qualified names denote the same entity",
// v0.307; stdlib §6.1 `CancellationToken`): companion to
// tests/codegen/qualified_builtin_threading_head_5012_5013_test.cpp.
//
// WHY A LIT TEST AND NOT A gtest ROW: the runtime VALUE of a MODULE-
// QUALIFIED `System.Threading.CancellationToken.None` does not distinguish
// fixed from unfixed — pre-fix, sema's `resolveMemberAccess` failed to
// recognize the qualified head, the call's `lowerExpr` returned null, and
// `lowerLocalDecl`'s #2423 defense-in-depth zero-init fallback stored a
// null pointer in the local's slot, which happens to be bit-identical to
// the CORRECT `.None` value for this one builtin. What DOES distinguish
// fixed from unfixed is the `internal: ... — #2434` Warning the fallback
// emits (present pre-fix, absent post-fix) — but `runAot`'s harness
// (tests/support/aot_support.hpp) unconditionally discards compiler
// stderr on a successful compile (`CompileOutcome::Ok`), so no gtest row
// can see it (see `scripts/check_internal_residual.py`'s docstring). A lit
// test drives the real `bezelc` CLI directly and inspects raw stderr
// regardless of outcome, so it is the correct — and only — mechanical
// oracle for this half of the fix. Modeled directly on
// tests/lit/local_decl_zero_init_fallback_warns_2434.bzl.
//
// ACCEPT-WITH-NO-WARNING: bezelc -c must exit zero (the program is legal
// both before and after this fix) AND must NOT emit the #2434 internal
// warning (pre-fix: it did; post-fix: sema now resolves the qualified head
// to the builtin `CancellationToken` symbol via `isQualifiedBuiltinThreading
// Head`, codegen's `.None` lowering recognizes that stamped symbol, and
// `lowerExpr` returns a real value, so the fallback path is never reached).
//
// A correctly-fixed compile emits NO diagnostics at all for this program, so
// stdin to FileCheck is legitimately empty — a pure CHECK-NOT with nothing
// else matches vacuously true on empty input in general, but FileCheck
// itself errors on a truly empty stream unless told to allow it (measured:
// "FileCheck error: '<stdin>' is empty" — same fix already used by
// tests/lit/ownership/await_using_yield_1677__not_live_at_yield_accepted.bzl
// and siblings, `--check-prefix=NODIAG --allow-empty`).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Threading;
class C
  public static method Main(): int
    var t: CancellationToken := System.Threading.CancellationToken.None;
    _ := t;
    return 0;
  end Main;
end C;

// NODIAG-NOT: internal: lowerLocalDecl's initializer expression failed to lower
