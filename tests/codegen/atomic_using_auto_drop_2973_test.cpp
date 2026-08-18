// Regression tests for issue #2973 (language spec §28.3 scope-bound auto-drop /
// §28.9 runtime-managed types; stdlib §6.4 `Atomic<T>` / `AtomicInteger<T>`,
// v0.240): a `using var` bound to an atomic emitted NO scope-exit release at
// all, so the instance leaked.
//
// #2965 (the closed-as-not-planned duplicate of #2973) is also pinned by this
// file — verified clean on HEAD (575f8cc) 2026-08-18, 8/8 tests pass under
// generated-code ASan.
//
//   using var a := new Atomic<int>(0);      // 4800 bytes / 200 allocations
//
// This is #2921's shape one receiver over: a built-in with a resolution on the
// EXPLICIT `a.Dispose()` path and none on the §28.3 auto-drop path.
//
// ── THE QUESTION THAT HAD TO BE SETTLED FIRST ───────────────────────────────
// #2973 flagged it explicitly: does `bz_atomic_dispose` FREE, or only MARK? If
// it only marked, the auto-drop would have needed a free as well — and adding
// one on top of a release that already frees is a double free, which is a worse
// outcome than the leak being fixed. Settled by READING THE RUNTIME
// (src/runtime/bezel_runtime_sync.cpp), not by inference: `bz_atomic_dispose`
// flips `disposed.exchange(true)` once and then calls `atomic_core_release`,
// which `delete`s the `bz_atomic_t` when the last core ref drops; `bz_atomic_new`
// hands back refs=1. It FREES. One call is a complete release.
//
// Exactly-once across the two spellings is structural rather than lucky: #2967
// made the explicit `a.Dispose()` arm NULL the receiver's slot, and the
// `vtablelessBuiltin` scope-exit path null-GUARDS its reload before releasing.
// `both_spellings_in_one_loop_is_not_a_double_free` pins that, because a leak is
// recoverable and a double free is not.
//
// ── MEASURED POPULATION, not an enumeration ─────────────────────────────────
// A 200-iteration `using var x := new T(…)` loop under generated-code LSan over
// every constructible built-in — Random, StringBuilder, Stopwatch, Mutex,
// Semaphore, CancellationTokenSource, EventSource, AsyncEventSource, Channel,
// TaskCompletionSource, Array, Dictionary, Set, SortedSet, SortedDictionary,
// Stack, Queue, LinkedList, OwningBuffer, Buffer — found EXACTLY TWO leaking:
// `Atomic<T>` and `AtomicInteger<T>`, 4800 bytes in 200 allocations each.
// Everything else is already claimed by an arm. So the fix is two names, and
// that is the whole live population rather than the first two found.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A 200-iteration loop is what #2973 measured with; anything that fails to
// release shows up as 200 allocations rather than one, so a stray single
// allocation elsewhere cannot be mistaken for the defect.
std::string loop(const char* body) {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do ") + body + " i := i + 1; end; "
        "  Console.WriteLine(\"done\"); "
        "  return 0; "
        "end Main; end C;";
}

}  // namespace

// ── The filed repro ─────────────────────────────────────────────────────────

BEZEL_TEST(atomic_using_auto_drop_2973,
           using_var_atomic_is_released_at_scope_exit_spec_28_3_stdlib_6_4) {
    // runAotChecked instruments the GENERATED code with ASan/LSan, so the leak
    // flips `.ran` to false. Without the fix this reports
    // "4800 byte(s) leaked in 200 allocation(s)".
    auto r = runAotChecked(loop("using var a := new Atomic<int>(0);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

BEZEL_TEST(atomic_using_auto_drop_2973,
           using_var_atomicinteger_is_released_at_scope_exit_stdlib_6_4) {
    // The sibling class. `AtomicInteger<T>` is NOT a subtype of `Atomic<T>`
    // (stdlib §6.4 says so explicitly), so it is a separate name with the same
    // backing and had to be measured separately — it leaked identically.
    auto r = runAotChecked(loop("using var a := new AtomicInteger<int>(0);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

BEZEL_TEST(atomic_using_auto_drop_2973,
           declared_type_spelling_is_also_released_spec_28_3) {
    // `registerUsingDisposal` probes three ways — the DECLARED type, the
    // NewExpr's type name, and the initializer's inferred exprType. The repro
    // exercises the second; this pins the first, which is the one a generic
    // factory or an annotated local takes.
    auto r = runAotChecked(
        loop("using var a: Atomic<int> := new Atomic<int>(0);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── A LEAK IS NOT THE WORST OUTCOME; A DOUBLE FREE IS ───────────────────────

BEZEL_TEST(atomic_using_auto_drop_2973,
           both_spellings_in_one_loop_is_not_a_double_free_spec_28_2) {
    // §28.2's exactly-once rule, across the two spellings that now BOTH
    // release. This was clean before the fix only because the auto-drop emitted
    // nothing at all; it stays clean after because #2967's explicit arm nulls
    // the slot and the scope-exit path null-guards its reload. If that ever
    // regresses, `bz_atomic_dispose` reads `a->disposed` through freed memory —
    // a use-after-free, which ASan catches here.
    auto r = runAotChecked(
        loop("using var a := new Atomic<int>(0); a.Dispose();"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

BEZEL_TEST(atomic_using_auto_drop_2973,
           explicit_dispose_alone_stays_clean_spec_28_2) {
    // The control #2973 reported as already clean. Pinned so the fix cannot be
    // "improved" into moving the release off the explicit path.
    auto r = runAotChecked(
        loop("var a := new Atomic<int>(0); a.Dispose();"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── The atomic must still WORK, not merely stop leaking ─────────────────────

BEZEL_TEST(atomic_using_auto_drop_2973,
           atomic_operations_still_produce_the_right_values_stdlib_6_4) {
    // The oracle is the VALUES, not exit 0: a release emitted too early would
    // still "not leak". stdlib §6.4: `Exchange` returns the PREVIOUS value;
    // `Add`/`Increment` return the POST-operation value.
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Atomic<int>(5); "
        "  a.Value := 11; "
        "  var old := a.Exchange(20); "
        "  var got := a.Value; "
        "  using var ai := new AtomicInteger<int>(0); "
        "  _ := ai.Add(3); "
        "  var n := ai.Increment(); "
        "  return old + got + n; "
        "end Main; end C;";
    auto r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(35, r.exitCode);   // 11 (previous) + 20 + 4 (3 then +1)
}

// ── ANTI-OVER-REJECT / anti-over-RELEASE control ────────────────────────────

BEZEL_TEST(atomic_using_auto_drop_2973,
           user_class_sharing_the_builtin_name_is_not_released_as_one_spec_11_3) {
    // A user class may legitimately be named `Atomic`. Freeing one of ITS
    // instances with `bz_atomic_dispose` would be memory-unsafe, and the
    // NewExpr detection probe reads an AST NAME, which by itself cannot tell
    // the two apart — so that probe is routed through the resolved TypeRef and
    // requires a declaration-less built-in. (The pre-#2973 spelling of the same
    // probe read the AST name bare; that shape is live elsewhere in this
    // function and is filed separately.) Oracle is the accumulated value.
    const std::string program =
        "module M; "
        "class Atomic public readonly property Tag: int := 9; end Atomic; "
        "class C public static method Main(): int "
        "  var i := 0; var sum := 0; "
        "  while i < 25 do "
        "    using var a := new Atomic(); "
        "    sum := sum + a.Tag; "
        "    i := i + 1; "
        "  end; "
        "  return sum; "
        "end Main; end C;";
    auto r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(225, r.exitCode);   // 25 * 9; kept inside the 8-bit exit code
}

BEZEL_TEST(atomic_using_auto_drop_2973,
           random_the_neighbour_2921_wired_is_unchanged_stdlib_3) {
    // This fix reworked #2921's arm from a hardcoded `"Random"` into a
    // name-parameterised one. `Random`'s own behaviour must be bit-identical —
    // pinned because the refactor is where a silent regression would hide.
    auto r = runAotChecked(loop("using var rr := new Random();"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}
