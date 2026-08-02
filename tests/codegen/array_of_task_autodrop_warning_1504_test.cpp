// Issue #1504 / spec §28.3 (v0.184 "Must-use warning") + §17.13 (v0.184
// "The discard `_`"): a plain-var owned `Array<Task>` local that is
// implicitly dropped at scope end is a *warning* (the auto-drop lint),
// NOT a hard error. Per §28.3 the rule is uniform across body kinds and
// the §28.3 must-use lint is emitted at the implicit-drop site; per
// §17.13 the only discards (and by transitivity the only scope-end
// auto-drops) that remain errors are bare `Task`/`Task<T>` (use
// `Task.FireAndForget` or `using var`). An `Array<Task>` holds the
// tasks as BORROWS (`Array<T>` is a borrowing collection, §5.11) —
// dropping the array abandons no task obligations. The compiler
// therefore must:
//   * emit the §28.3 must-use warning naming `a`
//   * still produce a binary (no hard error)
//
// Pre-fix, an `Array<Task>` plain-var local (the `w2.bz` repro from
// the 2026-07-07 spec/design review) was a hard error instead — fail
// with exit 1, no binary produced — contradicting §28.3's "warning
// (not an error)" text.
//
// Locks in the lint-form behavior. Mirrors the language surface in
// tests/codegen/owned_task_collection_1889's "borrowing array of
// tasks" usage, which routinely compiles with the §28.3 auto-drop
// warning.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Compile-and-check helper. Returns whether the program ran (binary
// produced), the exit code, and whether the "would be dropped here
// without disposal" hard error was emitted on `a` (the §1504 pre-fix
// shape). Note: stderr-emitted compiler warnings (including the §28.3
// auto-drop *lint* on `a`) are NOT available on the runAot success
// path — the driver discards compiler stderr after a successful
// compile+link (see aot_support.hpp's CompileOutcome::Ok path). The
// SPEC-PRESCRIBED correctness criterion is observable through the
// ran/exitCode/hardErrorOnA triple: a successful binary build at all
// IS the "warning, not error" verdict the spec mandates. Pre-fix
// #1504, the build was a Rejected — `r.ran == false` and the
// captured stderr surfaced the "would be dropped" hard error.
struct CompileCheck {
    bool ran = false;
    int exitCode = 0;
    bool hardErrorOnA = false;
};

CompileCheck check(const std::string& prog) {
    CompileCheck r;
    auto run = runAot(prog);
    r.ran = run.ran;
    r.exitCode = run.exitCode;
    for (const auto& d : run.diagnostics.diagnostics()) {
        const std::string& msg = d.message;
        if (d.severity == bezel::Severity::Error &&
            msg.find("would be dropped here without disposal") !=
                std::string::npos &&
            msg.find("`a`") != std::string::npos) {
            // The pre-#1504 hard error: "owned value in `a` would be
            // dropped here without disposal". Must NOT fire on an
            // `Array<Task>` plain-var auto-drop site — the spec
            // prescribes a §28.3 auto-drop *warning*, not an error.
            r.hardErrorOnA = true;
        }
    }
    return r;
}

}  // namespace

// §28.3 / #1504: the issue's exact repro — `var a := new Array<Task>();`
// + `a.Add(new Task<int>())`. The plain-var owned `Array<Task>` MUST
// be a warning, NOT a hard error. The binary MUST be produced (the
// spec's "auto-drop runs regardless" — the warning is a lint over a
// sound program).
BEZEL_TEST(codegen_array_of_task_autodrop_1504,
           plain_var_array_of_task_lints_not_errors) {
    auto r = check(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    var a := new Array<Task>(); "
        "    a.Add(new Task<int>()); "
        "    return 0; "
        "  end Main; "
        "end App;");
    // §28.3: warning, NOT error, on the plain-var owned Array<Task>.
    BEZEL_EXPECT_TRUE(r.ran);                              // binary produced
    BEZEL_EXPECT_FALSE(r.hardErrorOnA);                    // NOT a hard error
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3 / #1504 — typed array: `var a := new Array<Task<int>>()` follows
// the same lint-only regime — the Array<Task<T>> is a borrowing
// collection holding borrows; dropping it abandons no task obligations
// and must therefore be a warning, not an error.
BEZEL_TEST(codegen_array_of_task_autodrop_1504,
           typed_array_of_task_lints_not_errors) {
    auto r = check(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    var a := new Array<Task<int>>(); "
        "    return 0; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.hardErrorOnA);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3 / §17.13 / #1504 — discard statement. Per §17.13 the discard
// form `_ := new Array<Task>();` IS permitted (Array<Task> is not a
// bare Task). A warning may or may not fire on the discard, but it
// must NEVER be a hard error. The spec text "The only discards that
// remain errors are `Task`/`Task<T>`" (§17.13) explicitly excludes
// Array<Task>.
BEZEL_TEST(codegen_array_of_task_autodrop_1504,
           discard_array_of_task_not_error) {
    auto r = check(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    _ := new Array<Task>(); "
        "    return 0; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.hardErrorOnA);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3 / §28.9 / #1504 — the ISSUE'S ACTUAL REPRO (the `w2.bz` probe from
// the 2026-07-07 spec/design review, preserved at
// /var/tmp/bezel-scratch/specrun/async-await/w2.bz): an owned `Array<Task>`
// local declared in an ASYNC method whose live range CROSSES an `await` —
// i.e. a suspend-local (annotateSuspendLocals), not the simpler non-crossing
// shape the cases above cover. This is a DISTINCT code path in
// src/ownership/Ownership.cpp: the general sync/non-suspend-local warning
// carves (the `!isAsync_` gate above, and the #1663 non-suspend-local async
// gate) both bypass a suspend-local, which instead reaches the SB01-C1/D2
// carve (#1786/#1868) — historically keyed ONLY to "is this local itself a
// Task", so a suspend-local `Array<Task>` (a class, not a Task) fell through
// to the hard §28.3 reject exactly as the issue reports, even though the
// non-async/non-suspend-crossing shapes above (already fixed pre-#1786) were
// long since fine. `Array<Task>` is a borrowing collection (§5.11) whose own
// `Dispose()` is trivial (§28.9 — it holds no owned elements, only borrowed
// `Task` references) — the "genuine frame-teardown gap" residual-leak
// concern that keeps a general disposable-class suspend-local hard-rejected
// (verified NOT to regress below) does not apply: there is nothing to lose
// if the frame is abandoned before resuming, because Dispose() releases
// nothing either way.
BEZEL_TEST(codegen_array_of_task_autodrop_1504,
           async_suspend_crossing_array_of_task_lints_not_errors) {
    auto r = check(
        "module M; "
        "class Program "
        "  public static async method Main(): Task "
        "    using var t := Task.Delay(1); "
        "    var a := new Array<Task>(); "
        "    a.Add(t); "
        "    await t; "
        "    Console.WriteLine($\"{a.Count}\"); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);             // binary produced (warning, not error)
    BEZEL_EXPECT_FALSE(r.hardErrorOnA);   // NOT the §28.3 hard discharge error
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3 / §28.9 / #1504 — the same async-suspend-local shape, ASan/LSan-
// checked: the array's own backing allocation must be reclaimed EXACTLY
// ONCE when the frame resumes past the `await` and `a` auto-drops at scope
// end. A leak (the pre-fix reject masked this question entirely — a program
// that never compiled cannot leak) or a double-free (an over-broad carve
// disposing an already-collection-owned or already-scope-end-disposed slot
// twice) would flip `r.ran` to false under runAotChecked.
BEZEL_TEST(codegen_array_of_task_autodrop_1504,
           async_suspend_crossing_array_of_task_disposed_exactly_once) {
    auto r = runAotChecked(
        "module M; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Task.Delay(1); "
        "    var a := new Array<Task>(); "
        "    a.Add(t); "
        "    await t; "
        "    return a.Count; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);       // ASan/LSan-clean: no leak, no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 1); // a.Count == 1 (the one Add'd task)
}

// §28.3 / §28.9 / #1504 (negative control) — a GENUINELY disposable class
// (a destructor — real cleanup to lose) as a suspend-local crossing an
// `await` must STAY the hard §28.3 reject: the fix is scoped to §28.9
// TRIVIAL-dispose suspend-locals (rhsIsDisposableClassType == false); a
// non-trivial-dispose class suspend-local's frame-teardown gap is a genuine
// residual leak risk (documented at the SB01-C1/D2 comment site in
// Ownership.cpp) that this fix must not silently paper over.
BEZEL_TEST(codegen_array_of_task_autodrop_1504,
           async_suspend_crossing_disposable_class_still_hard_rejected) {
    auto run = runAot(
        "module M; "
        "class Resource "
        "  destructor end destructor; "
        "end Resource; "
        "class Program "
        "  public static async method Main(): Task "
        "    using var t := Task.Delay(1); "
        "    var r := new Resource(); "
        "    await t; "
        "  end Main; "
        "end Program;");
    bool hardErrorOnR = false;
    for (const auto& d : run.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("would be dropped here without disposal") !=
                std::string::npos &&
            d.message.find("`r`") != std::string::npos) {
            hardErrorOnR = true;
        }
    }
    BEZEL_EXPECT_FALSE(run.ran);   // still rejected — no binary
    BEZEL_EXPECT_TRUE(hardErrorOnR);
}
