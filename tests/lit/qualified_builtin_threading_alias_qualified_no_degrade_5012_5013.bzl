// fix #5012, #5013 (spec §2.7 "Qualified names denote the same entity",
// v0.307; stdlib §6.1 `CancellationToken`): companion to
// tests/codegen/qualified_builtin_threading_head_5012_5013_test.cpp and to
// tests/lit/qualified_builtin_threading_module_qualified_no_degrade_5012_5013.bzl
// (that file's header explains why this must be a lit test, not a gtest
// row: the runtime VALUE of `CancellationToken.None` cannot distinguish
// fixed from unfixed, only the #2434 internal warning's presence/absence
// can, and `runAot` discards compiler stderr on a successful compile).
//
// This file is the ALIAS-qualified counterpart: `import System.Threading
// as T;` then `T.CancellationToken.None`, exercising the OTHER arm of
// `ExprTC::isQualifiedBuiltinThreadingHead` (the `IdentifierExpr` alias
// arm, guarded by `!lookupName(aid->name)` per §2.7 rule 1 so a local
// shadowing the alias name is never mistaken for it — see this commit's
// gtest file for the shadowing accept-controls that assert VALUES, which
// this diagnostic-only oracle cannot).
//
// ACCEPT-WITH-NO-WARNING: bezelc -c must exit zero and must NOT emit the
// #2434 internal warning (pre-fix: it did, via the same zero-init
// fallback as the module-qualified case; post-fix: the alias arm resolves
// `T.CancellationToken` to the builtin symbol just like the module-
// qualified arm does).
//
// A correctly-fixed compile emits NO diagnostics at all for this program, so
// stdin to FileCheck is legitimately empty — FileCheck errors on a truly
// empty stream unless told to allow it (measured: "FileCheck error:
// '<stdin>' is empty" — same fix already used by
// tests/lit/ownership/await_using_yield_1677__not_live_at_yield_accepted.bzl
// and siblings, `--check-prefix=NODIAG --allow-empty`).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Threading as T;
class C
  public static method Main(): int
    var t: CancellationToken := T.CancellationToken.None;
    _ := t;
    return 0;
  end Main;
end C;

// NODIAG-NOT: internal: lowerLocalDecl's initializer expression failed to lower
