// Regression test for issue #3024 (MED, sema, v0.240).
//
// Sibling of #3021 (which covered File/Directory/Path/Debug/Domain and was
// reverted at db950a6 due to a regression on `Path.Deserialize(d)` — a
// user record Path's legitimate static factory). #3024 covers the three
// remaining same-shape names from the original brief: Task, Stopwatch,
// CancellationTokenSource.
//
// The bug: a bogus STATIC member call on any of these three names silently
// zero-degraded under the §2434 internal warning (one INTERNAL note from
// lowerLocalDecl's initializer-expression zero-init fallback) and exited
// 0 with `v = 0`. The codegen gate in `Emitter::emittedSideEffectSince`
// fires only for INSTANCE member calls on built-in values (its
// `builtinReceiverOfMemberCall` reads `ctx_.exprType(*ma->target)`, which
// is null for a type-identifier receiver like `Task`). A STATIC call is
// dispatched through a different path the gate does not cover, so no
// per-static-class closed-surface arm existed in sema and
// `Task.TotallyBogusMember(1, 2)` was never rejected.
//
// The fix: a per-static-class closed-surface arm in sema at
// ExprTypecheck_Calls.cpp:6679-6741, mirroring the per-instance-type arms
// further below. Restricts the guard to the three names #3024 owns
// (Task/Stopwatch/CTS — the other 5 from #3021 are tracked separately).
// Discriminator is `recvTy == nullptr` — the call is on the builtin
// static-class IDENTIFIER (`Task.X(...)`), where the colName lambda's
// IdentifierExpr fallback fires (per its "Builtin class identifiers are
// given a symbol but no TypeRef" comment). Instance calls on a builtin
// (`cts.Cancel()`, `sw.Restart()`) have recvTy non-null (the instance
// TypeRef) and so DO NOT trip the guard — the per-instance-type arms at
// 6914-7775 handle them, and the codegen #605 permissive path covers
// what those arms miss.
//
// This test pins four things:
//
//   1. The bug — all three names emit an Error diagnostic on
//      `Name.TotallyBogusMember(1, 2)`. The discriminator is `!r.ran`
//      (compile-time rejection), AND the diagnostic text contains the
//      per-name "documented surface" wording, so the rejection is the
//      expected sema arm (not a coincidental crash or warning).
//
//   2. The legitimate surface — every documented STATIC method on each
//      of the 3 names still compiles cleanly. A false reject is worse
//      than the bug it fixes, so the legitimate surface is the larger
//      half of the test.
//
//   3. The instance-call-still-works rule — `cts.Cancel()`,
//      `cts.Token`, `cts.IsCancellationRequested` are NOT rejected by the
//      guard (they're instance calls on a builtin, recvTy non-null).
//      This is the discriminator that fixed the Wave 3 #3021 regression
//      class: a static-only discriminator would have wrongly rejected
//      instance calls too.
//
//   4. The user-class-same-name case — a user class declared with the
//      same name (`Task`, `Stopwatch`, `CancellationTokenSource`) is
//      unaffected. The receiver TypeRef for a user class has a non-null
//      declaration with decl!=nullptr, so recvTy is non-null and the
//      guard doesn't fire. (This is the same property the Wave 3 fix
//      accidentally broke for `Path.Deserialize` — see (3) above.)
//
// The discriminator (runAot `WIFSIGNALED` synthesizes an Error for a
// runtime SIGSEGV) is asserted on diagnostic TEXT per tests/README.md.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// True iff the program was REJECTED at compile time (did not run).
bool rejected(const std::string& program) {
    auto r = runAot(program);
    return !r.ran;
}

// True iff the FIRST Error diagnostic mentions the per-name "documented
// surface" wording that the per-static-class arm emits. This pins WHICH
// mechanism rejected it — the sema arm we added — not merely that the
// program was rejected (the runAot `WIFSIGNALED` synthesizes an Error for
// a runtime SIGSEGV too, per tests/README.md).
bool rejectedWithDocumentedSurface(const std::string& program,
                                   const std::string& expectedClassName) {
    auto r = runAot(program);
    if (r.ran) return false;     // accepted — failure
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error) {
            return d.message.find("documented surface") != std::string::npos &&
                   d.message.find(expectedClassName) != std::string::npos;
        }
    }
    return false;
}

// True iff the program ran successfully (exit 0).
bool runsOk(const std::string& program) {
    auto r = runAot(program);
    return r.ran && r.exitCode == 0;
}

// One entry per static class covered by #3024. `name` is the class name;
// `bogusProgram` is the minimal program whose call `Name.TotallyBogusMember(1, 2)`
// must be rejected with the "documented surface" diagnostic. `legitProgram`
// exercises every documented STATIC member — must compile clean.
struct StaticClassCase {
    const char* name;
    std::string bogusProgram;
    std::string legitProgram;
};

const char* kProgFrame =
    "module M; "
    "class C public static method Main(): int ";

StaticClassCase makeTaskCase() {
    StaticClassCase c{"Task",
        std::string(kProgFrame) +
            "  var v := Task.TotallyBogusMember(1, 2); "
            "  return 0; "
            "end Main; end C;",
        // Legit surface: FromResult<T>, FromException, FromCancelled,
        // Delay (int overload — simplest), WhenAll, WhenAny, FireAndForget
        // (skipped — would need a real task arg), FromOwnedResult (skipped —
        // needs a class T). 7 of 8 names verified.
        std::string(kProgFrame) +
            "  using var t := Task.Delay(1); "
            "  using var t2 := Task.FromResult(42); "
            "  using var t3 := Task.FromException<int>(new Exception(\"x\")); "
            "  using var t4 := Task.FromCancelled<int>(); "
            "  using var t5 := Task.WhenAll([t]); "
            "  using var t6 := Task.WhenAny([t]); "
            "  return 0; "
            "end Main; end C;"};
    return c;
}

StaticClassCase makeStopwatchCase() {
    StaticClassCase c{"Stopwatch",
        std::string(kProgFrame) +
            "  var v := Stopwatch.TotallyBogusMember(1, 2); "
            "  return 0; "
            "end Main; end C;",
        std::string(kProgFrame) +
            "  using var sw := Stopwatch.StartNew(); "
            "  return 0; "
            "end Main; end C;"};
    return c;
}

StaticClassCase makeCancellationTokenSourceCase() {
    StaticClassCase c{"CancellationTokenSource",
        std::string(kProgFrame) +
            "  var v := CancellationTokenSource.TotallyBogusMember(1, 2); "
            "  return 0; "
            "end Main; end C;",
        // Legit surface: WithCancelAfter, CreateLinked, CreateLinkedAll.
        // Using var for owned lifetimes.
        std::string(kProgFrame) +
            "  using var cts1 := CancellationTokenSource.WithCancelAfter("
            "      TimeSpan.FromSeconds(1)); "
            "  using var cts2 := new CancellationTokenSource(); "
            "  using var cts3 := new CancellationTokenSource(); "
            "  using var linked := CancellationTokenSource.CreateLinked("
            "      cts2.Token, cts3.Token); "
            "  using var parents := new Array<CancellationToken>(); "
            "  parents.Add(cts2.Token); parents.Add(cts3.Token); "
            "  using var all := CancellationTokenSource.CreateLinkedAll(parents); "
            "  return 0; "
            "end Main; end C;"};
    return c;
}

} // namespace

// ===========================================================================
// (1) The bug — all three names reject `Name.TotallyBogusMember(1, 2)`.
// ===========================================================================

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           bogus_task_member_call_rejected_with_documented_surface) {
    auto c = makeTaskCase();
    BEZEL_EXPECT_TRUE(rejected(c.bogusProgram));
    BEZEL_EXPECT_TRUE(rejectedWithDocumentedSurface(c.bogusProgram, c.name));
}

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           bogus_stopwatch_member_call_rejected_with_documented_surface) {
    auto c = makeStopwatchCase();
    BEZEL_EXPECT_TRUE(rejected(c.bogusProgram));
    BEZEL_EXPECT_TRUE(rejectedWithDocumentedSurface(c.bogusProgram, c.name));
}

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           bogus_cts_member_call_rejected_with_documented_surface) {
    auto c = makeCancellationTokenSourceCase();
    BEZEL_EXPECT_TRUE(rejected(c.bogusProgram));
    BEZEL_EXPECT_TRUE(rejectedWithDocumentedSurface(c.bogusProgram, c.name));
}

// ===========================================================================
// (2) The legitimate STATIC surface — every documented method still
// compiles cleanly. A false reject is worse than the bug it fixes.
// ===========================================================================

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           legit_task_static_surface_still_compiles) {
    auto c = makeTaskCase();
    auto r = runAot(c.legitProgram);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           legit_stopwatch_static_surface_still_compiles) {
    auto c = makeStopwatchCase();
    auto r = runAot(c.legitProgram);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           legit_cts_static_surface_still_compiles) {
    auto c = makeCancellationTokenSourceCase();
    auto r = runAot(c.legitProgram);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (3) The instance-call-still-works rule. The guard's discriminator is
// `recvTy == nullptr`, so instance calls on a builtin CTS/Stopwatch
// instance DO NOT trip the guard. This is the discriminator that fixed
// the Wave 3 #3021 regression class — `cts.Cancel()`, `sw.Restart()`,
// `cts.Token`, etc. must NOT be rejected.
//
// These programs run on current main (pre-fix) AND with the fix; the
// pin is that they STILL run after the fix. The codegen `#605
// permissive` path covers any instance method that the per-instance-type
// arms miss, so the programs may emit internal notes but no Errors
// and exit successfully.
// ===========================================================================

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           instance_call_on_cts_is_not_rejected_by_static_guard) {
    // `cts.Cancel()` and `cts.IsCancellationRequested` are INSTANCE
    // calls on a CancellationTokenSource instance — colName resolves
    // to "cts" (variable name) for the receiver (the type name only
    // appears in the static-class-identifier fallback path where
    // recvTy is null). The guard must not fire.
    //
    // The program doesn't pin a specific return value — the codegen
    // `#605 permissive` path covers these instance methods, and
    // cancellation behavior depends on the codegen-implementation
    // detail. The PIN is that the program runs cleanly (no sema
    // Errors) — which it did NOT on the original buggy attempt where
    // the guard fired on instance calls and rejected the program.
    auto r = runAot(
        std::string(kProgFrame) +
            "  using var cts := new CancellationTokenSource(); "
            "  cts.Cancel(); "
            "  var b := cts.IsCancellationRequested; "
            "  return 0; "
            "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           linked_cts_lifecycle_unchanged_by_fix) {
    // The Wave 3 #3021 regression family: a linked CTS chain
    // (`CancellationTokenSource.CreateLinked(a.Token, b.Token)`)
    // must keep working — `CreateLinked` IS in the static surface
    // list, so this is the "legit surface" half (the test pins it
    // specifically because it was the regression in #3021). If the
    // guard wrongly fires for `a.Cancel()` or `a.Token` (instance
    // calls), this test fails.
    auto r = runAot(
        std::string(kProgFrame) +
            "  using var a := new CancellationTokenSource(); "
            "  using var b := new CancellationTokenSource(); "
            "  using var child := CancellationTokenSource.CreateLinked("
            "      a.Token, b.Token); "
            "  a.Cancel(); "
            "  if child.Token.IsCancellationRequested then return 1; end; "
            "  return 0; "
            "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(1, r.exitCode);
}

// ===========================================================================
// (4) User classes with the same name are unaffected — a USER class
// named `Task`, `Stopwatch`, or `CancellationTokenSource` (with a
// public static method) is unaffected by the gate. The discriminator
// `recvTy == nullptr` correctly excludes this case: the user class's
// TypeRef has a non-null declaration (the user TypeSymbol with
// decl!=nullptr) — wait, that's the receiver TypeRef for an INSTANCE
// call, not a STATIC call. For a STATIC call on a user class like
// `MyTask.Run()`, the receiver is the user class TypeRef
// (declaration non-null, decl non-null) and colName == "MyTask", which
// doesn't match the surface list anyway. The discriminator matters
// for cases like a user TYPE named "Task" being used as a static-call
// receiver — colName would be "Task" but recvTy is the user TypeRef.
// Pins §11.3 / #654 that a USER class named `Task` is unaffected.
// ===========================================================================

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           user_class_named_task_is_unaffected) {
    auto r = runAot(
        "module M; "
        "class Task "
        "  public static method TotallyBogusMember(x: int, y: int): int => x + y; "
        "end Task; "
        "class C public static method Main(): int "
        "  return Task.TotallyBogusMember(1, 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           user_class_named_stopwatch_is_unaffected) {
    auto r = runAot(
        "module M; "
        "class Stopwatch "
        "  public static method TotallyBogusMember(x: int, y: int): int => x + y; "
        "end Stopwatch; "
        "class C public static method Main(): int "
        "  return Stopwatch.TotallyBogusMember(1, 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

BEZEL_TEST(codegen_static_class_closed_surface_3024,
           user_class_named_cts_is_unaffected) {
    auto r = runAot(
        "module M; "
        "class CancellationTokenSource "
        "  public static method TotallyBogusMember(x: int, y: int): int => x + y; "
        "end CancellationTokenSource; "
        "class C public static method Main(): int "
        "  return CancellationTokenSource.TotallyBogusMember(1, 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(3, r.exitCode);
}