// Spec-derived regression test for #3961 — a string produced through a
// `Task<string>` result is never released; the SOLE +1 the producer's
// `return <inner-string>;` transferred into the task result slot has no
// matching release on the consumer side.
//
// §30.3 / §30.5 (v0.295): `<task>.Result` on a `Task<string>` is a
// synchronous read; the runtime's `bz_task_complete` memcpy's the string
// pointer into the task result buffer without a retain (no refcount change,
// bezel_runtime_concurrency.cpp:737), so the consumer's `var s := t.Result`
// slot is the SOLE owner of the producer's +1 once the producer's resume
// returns. Before this fix the bind-time path classified `t.Result` as a
// borrow (the `MemberAccessExpr` `isPlusOneStringProducer` arm did not
// recognize a builtin-task `.Result` read), so the consumer's slot
// retained an extra +1 the producer never had, then released it at scope
// end — net zero from the consumer side, but the producer's single +1 held
// by the task result slot leaked on every iteration. Measured as 18500 B /
// 1000 allocations on 500 iterations of the issue's repro, surfacing in
// LSan as direct (16000 B) + indirect (2500 B) leak stacks rooted at the
// producer's resume (`bz$async$M.C$Make$Resume` → `bz.malloc`).
//
// The fix extends `isPlusOneStringProducer` in
// `src/codegen/LlvmBackend_Setup.cpp` to recognize `<task>.Result` on a
// `Task<string>` (non-nullable) receiver as a +1 string producer, so the
// bind-time `armStringLocalRefcount` skips the bind-time retain (the
// producer's +1 flows into the slot) and arms a scope-end release that
// drops the producer's +1 exactly once. The fix is intentionally narrow:
// the `OwningTask<T>` / `Task<class-T>` consumer side (Pattern A class
// results) is owned by `bz_task_free`'s disposer cascade (#1503) and is
// explicitly excluded; a `string?` `Result` has no producer contract and
// stays on the existing borrow path; the `await f()` (Pattern A) twin
// goes through its own disposer path (#3070 / #3099) and is untouched.
// per §30.3 / §30.5 (v0.295), §4.6

#include "support/aot_support.hpp"     // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// §30.3 / §30.5 / #3961 (v0.295): the issue's own repro — a `string` produced
// through a `Task<string>` result leaks 18500 B / 1000 allocs at 500
// iterations because no ownership edge drops the producer's +1. Without the
// fix `runAotChecked` reports `r.ran == false` with an LSan leak summary;
// with the fix it reports the loop's `4000` and `r.ran == true`.
BEZEL_TEST(async_task_string_result_release_3961,
            issue_repro_500_iterations_clean) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static async method Make(): Task<string> "
        "    var inner := \"ab\" + \"cd\"; "
        "    return inner; "
        "  end Make; "
        "  public static method Run(): int "
        "    using var t := C.Make(); "
        "    t.Wait(); "
        "    var f := t.Result; "
        "    return f.Length; "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      total := total + C.Run(); "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §30.3 / #3961: a single-shot equivalent of the loop test, isolating the
// fix from any amplification drift. The print of the captured result both
// exercises the borrowed read (so the consumer's slot is alive across the
// print) and verifies no under-release went undetected by ASan (which would
// not catch a leak at single iter). Uses `bz_string_t.Length` rather than
// `Console.WriteLine` for the second read so the consumer's release is
// exercised even if `WriteLine` releases for itself.
BEZEL_TEST(async_task_string_result_release_3961,
            single_shot_through_then_cross_use_clean) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static async method Make(): Task<string> "
        "    var inner := \"ab\" + \"cd\"; "
        "    return inner; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var t := C.Make(); "
        "    t.Wait(); "
        "    var s := t.Result; "
        "    Console.WriteLine(s); "
        "    var second := s.Length; "
        "    return second; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // "abcd".Length == 4
}

// §30.3 / #3961: control — `Task<int>` (value-type Result). Pre-fix this case
// is leak-free (value types have no heap refcount); post-fix the new producer
// arm GATED on `string` T must be a no-op here, so this guards the gating.
BEZEL_TEST(async_task_string_result_release_3961,
            control_task_int_value_result_clean) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static async method Make(): Task<int> "
        "    return 42; "
        "  end Make; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      using var t := C.Make(); "
        "      t.Wait(); "
        "      total := total + t.Result; "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

} // namespace
