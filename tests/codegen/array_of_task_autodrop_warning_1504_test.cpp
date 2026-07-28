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
