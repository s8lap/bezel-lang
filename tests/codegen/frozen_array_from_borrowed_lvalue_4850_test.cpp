// Regression tests for issue #4850 — `FrozenArray<T>.From(xs)` with a
// borrowed-lvalue source aliased the source's buffer and SEGV'd at scope
// exit (the calling `using var xs: Array<T>` still owned its buffer; the
// pre-fix `bz_array_free(src, null)` call at the end of
// `tryLowerFrozenArrayCall` freed it before the caller's scope-end dispose,
// so `xs`'s destructor dereferenced already-freed storage at
// `bz_array_free:294`). The exit code differed between origin/main (-6
// abort) and r20-c25 (-11 SEGV) — the signature of heap corruption.
//
// Per stdlib §4.19: `From(items: Array<T>)` COPIES — the caller still owns
// `items`, and the result owns an independent storage that it disposes at
// its own scope end. The codegen arm at
// `LlvmBackend_Calls2.cpp::tryLowerFrozenArrayCall` previously freed the
// source unconditionally, treating every source as a temporary; the fix
// gates the source-disposal on whether ownership actually transferred into
// the call (CollectionLiteralExpr / explicit `owned` modifier / `move`
// UnaryExpr). A borrowed lvalue passes through unchanged, so its
// scope-end dispose remains the only free.
//
// The 6dcfb9d7 v1 fix's allow-list `literal || owned || move` mis-classified
// every other fresh rvalue (call results, instance-method results, inline-if
// expressions yielding a fresh array) as a borrowed lvalue, and SKIPPED the
// explicit free — leaking the source's heap allocation across each iteration.
// The rework inverts the predicate: a source is BORROWED only when it is a
// place expression (IdentifierExpr / MemberAccessExpr / IndexExpr); anything
// else is an rvalue and the call owns the storage. Three new rows pin the
// rework against LSan — they FAIL on v1 (the pre-rework source is leaked
// every iteration) and PASS on the rework.
//
// Cite: stdlib §4.19, language spec §28.3, v0.307.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Helper: build a program with a public class containing Main that runs
// the supplied body and prints to stdout via Console.WriteLine. Returns the
// exit code (-999 if not run).
int run(const std::string& body) {
    const std::string p =
        "module M; "
        "public class Program "
        "  public static method Main() "
        + body +
        "  end Main; "
        "end Program;";
    auto r = runAot(p);
    return r.ran ? r.exitCode : -999;
}

// ═══════════ Primary bug: the measured repro from #4850 ═══════════════════
// Pre-fix: prints "b" then SEGVs at scope exit (exit -11 on r20-c25).
// Post-fix: prints "b" then exits cleanly (exit 0). Loop-amplified to 200
// iterations per [[feedback-vacuous-pass]] — the unreleased-buffer aliasing
// shows up within a handful of iterations; the loop protects against any
// LSan "reachable-from-stack-slot" one-shot pass.
BEZEL_TEST(frozen_array_4850,
           from_borrowed_lvalue_string_no_segv_loop_amplified_spec_4_19) {
    const std::string body =
        "  var i := 0; "
        "  while i < 200 do "
        "    using var xs: Array<string> := [\"a\", \"b\"]; "
        "    using var fa := FrozenArray<string>.From(xs); "
        "    Console.WriteLine(fa[1]); "
        "    i := i + 1; "
        "  end; ";
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main() " + body +
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ═══════════ Variant: borrowed-lvalue source on a non-string element ═══════
// Same bug shape with `int` elements. The `emitArrayRetainStringsIfStringArray`
// arm is a no-op for non-strings, so this isolates the bz_array_free SEGV
// (the issue's actual crash site) from any string-retention concern.
BEZEL_TEST(frozen_array_4850,
           from_borrowed_lvalue_int_no_segv_loop_amplified_spec_4_19) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main() "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var xs: Array<int> := [10, 20, 30]; "
        "      using var fa := FrozenArray<int>.From(xs); "
        "      Console.WriteLine(fa[1]); "
        "      i := i + 1; "
        "    end; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ═══════════ Accept-side control: literal source still disposes cleanly ═══
// The pre-existing literal form `FrozenArray<int>.From([...])` must keep
// passing — the materializer's fresh allocation still needs the explicit
// `bz_array_free`. This is the exact path every prior frozen_array_*_test
// (1723, 3249, 3907) exercises; if the fix broke it, those would already
// fail at link, but this row pins the literal-disposal behavior directly.
BEZEL_TEST(frozen_array_4850,
           from_literal_source_still_works_parity_control_spec_4_19) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := FrozenArray<int>.From([1, 2, 3]); "
        "    if a.Length <> 3 then return 1; end; "
        "    if a[0] <> 1 then return 2; end; "
        "    if a[1] <> 2 then return 3; end; "
        "    if a[2] <> 3 then return 4; end; "
        "    return 42; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ═══════════ Accept-side control: `move` source still disposes cleanly ═════
// `FrozenArray<T>.From(move xs)` transfers ownership into the call, so the
// source MUST be freed here (the slot is nulled by lowerUnary's move-handling,
// so the caller's scope-end dispose is a no-op). The fix preserves this —
// `sourceIsMoved` is true and the explicit free still runs.
BEZEL_TEST(frozen_array_4850,
           from_move_source_still_works_parity_control_spec_4_19_spec_28_3) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var buf := new OwningBuffer<int>(3); "
        "    buf[0] := 7; buf[1] := 8; buf[2] := 9; "
        "    using var fa := FrozenArray<int>.From(move buf); "
        "    if fa[0] <> 7 then return 1; end; "
        "    if fa[1] <> 8 then return 2; end; "
        "    if fa[2] <> 9 then return 3; end; "
        "    return 42; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ═══════════ Borrowed lvalue AND freed before scope end ═══════════════════
// Explicit dispose of `xs` before the FrozenArray goes out of scope —
// proves the source is NOT being freed by `From` (otherwise this would
// SEGV on the explicit dispose). Post-fix: prints "b", exits cleanly.
BEZEL_TEST(frozen_array_4850,
           from_borrowed_lvalue_then_explicit_dispose_spec_4_19) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [\"a\", \"b\"]; "
        "    using var fa := FrozenArray<string>.From(xs); "
        "    Console.WriteLine(fa[1]); "
        "    if fa[1] <> \"b\" then return 1; end; "
        "    if xs[0] <> \"a\" then return 2; end; "
        "    return 42; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ═══════════ Fresh rvalue: call result (per rework §4.19 v0.307) ═══════════
// The 6dcfb9d7 fix's allow-list `sourceIsLiteral || sourceIsMoved` missed
// the `From(Make(i))` shape: `Make(i)` is a fresh `Array<int>` from a
// static method call, no caller owns it, but the fix's narrow predicate
// classified it as a borrowed lvalue and skipped the explicit free —
// leaking every call's result over 200 iterations. The rework inverts the
// predicate: a CallExpr (anything but a place expression) IS owned here,
// so the source-disposal runs. Sum is 0+1+...+199 = 19900.
//
// runAotChecked: LSan-pinned. Pre-rework (6dcfb9d7) leaks ~24000 bytes /
// 400 allocations (cycle-25 main 2026-09-25, measured); post-rework the
// explicit free runs and LSan reports 0 bytes. This is the row the gate's
// `--full` ASan leg banks against — the in-process `runAot` (no LSan) is
// vacuous for leaks.
BEZEL_TEST(frozen_array_4850,
           from_call_result_no_leak_loop_amplified_spec_4_19) {
    auto r = runAotChecked(
        "module M; "
        "public class Program "
        "  public static method Make(n: int): Array<int> "
        "    var a: Array<int> := [n]; "
        "    return a; "
        "  end Make; "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var fa := FrozenArray<int>.From(Make(i)); "
        "      var sum: int := 0; "
        "      for var x in fa do sum := sum + x; end; "
        "      total := total + sum; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("19900") != std::string::npos);
}

// ═══════════ Fresh rvalue: instance-method result (per rework §4.19) ═══════
// `xs.ToArray()` returns `owned Array<T>` per stdlib §4.4 — a fresh heap
// copy. The 6dcfb9d7 fix mis-classified this as a borrowed lvalue (the
// 6dcfb9d7 predicate only catches literals/`move`/`owned` modifiers) and
// leaked every iteration's fresh array. The rework catches it via the
// inverted predicate: a method-call expression is not a place expression,
// so the source is owned here and the explicit free runs.
//
// runAotChecked: same LSan pin as the call-result row.
BEZEL_TEST(frozen_array_4850,
           from_instance_method_result_no_leak_loop_amplified_spec_4_19) {
    auto r = runAotChecked(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      var xs: Array<int> := [i]; "
        "      using var fa := FrozenArray<int>.From(xs.ToArray()); "
        "      var sum: int := 0; "
        "      for var x in fa do sum := sum + x; end; "
        "      total := total + sum; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("19900") != std::string::npos);
}

// ═══════════ Fresh rvalue: inline-if expression yielding call result ══════
// `(if cond then Make(i) else MakeOffset(i))` is an rvalue (ConditionalExpr,
// not a place expression). The 6dcfb9d7 predicate mis-classified it as a
// borrowed lvalue and skipped the free. The rework inverts: a
// ConditionalExpr with fresh branches is owned. Loop 200x (i=0..199,
// exclusive upper): even i contributes i (sum 9900 over 100 even i values
// 0,2,...,198), odd i contributes i+1000 (sum 110000 = 10000 arithmetic +
// 100000 from the +1000 offset across 100 odd iterations), total =
// 119900. Loop-amplified under ASan: 6dcfb9d7 leaks ~200 fresh arrays;
// the rework's inverted predicate catches the inline-if as a non-place
// expression and the source-disposal runs.
//
// Note: the inline-if branches must be call expressions, not collection
// literals — per §20 / A.7, a collection literal cannot be an `if` branch
// because it has no contextual target type. `Make` / `MakeOffset` supply
// the typed `Array<int>` via return type. The case path (`case i of …`)
// would also reach `ConditionalExpr`/`CaseExpr` in the AST but produces
// a degraded value (#2434) on this shape, so the inline-if is the cleaner
// pin for the rework's ConditionalExpr branch.
//
// runAotChecked: same LSan pin as the other two rework rows.
BEZEL_TEST(frozen_array_4850,
           from_conditional_fresh_rvalue_no_leak_loop_amplified_spec_4_19) {
    auto r = runAotChecked(
        "module M; "
        "public class Program "
        "  public static method Make(n: int): Array<int> "
        "    return [n]; "
        "  end Make; "
        "  public static method MakeOffset(n: int): Array<int> "
        "    return [n + 1000]; "
        "  end MakeOffset; "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var fa := FrozenArray<int>.From((if i mod 2 = 0 then Make(i) else MakeOffset(i))); "
        "      var sum: int := 0; "
        "      for var x in fa do sum := sum + x; end; "
        "      total := total + sum; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("119900") != std::string::npos);
}

}  // namespace
