// fix #7870 (§12.7 / §30.3 / §30.4, v0.353) — `t is Task` over a
// `Task<string>` must be TRUE: §30.3 declares `public class Task<T>: Task`, so
// every `Task<T>` IS a `Task`.
//
// ── THE DEFECT ──────────────────────────────────────────────────────────────
//
// `Emitter::lowerTypeTest` (src/codegen/LlvmBackend_Layout.cpp) answers a
// type test from the TARGET's runtime descriptor. A Task has none: a
// `bz_task_t*` is a runtime struct with no Bezel vtable at word 0, so
// `boxDescriptorFor` / `descArray_` / `builtinDesc_` all miss, and the test
// reached the `!targetDesc` bail at the end of the function, which reported
// and DROPPED it. Pre-fix, over a `Task<string>`:
//
//     probe: var t := S();  Console.WriteLine((t is Task).ToString());
//     warning: internal: this type test was DROPPED and reads as false — the
//              built-in class `Task` has no runtime type tag … — #8000
//     warning: internal: the argument of this `Console.WriteLine` failed to
//              lower to a value, so the whole statement emits nothing — #3079
//
// ⇒ the program printed NOTHING AT ALL (not even "False"), and exited 0 — the
// silent-wrong-value shape #2434's family records. MEASURED PRE-FIX (base
// c05c35bf, `build/bezelc`): every one of the five type tests in the probe
// emitted no line:
//
//     (t is Task)        → dropped (#8000) → no line
//     (t is Task<string>) → dropped        → no line
//     (t is Task<int>)    → dropped        → no line
//     (t is Task?)        → dropped (#8000) → no line
//     (t is not Task)     → dropped (#8000) → no line
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// The test IS decidable statically here, and §12.7 says how: "A test naming
// the operand's own type is admitted: statically true", and the operand's
// static type here is a Task-family type. The relations are §30.3's:
//
//     public class Task          public readonly property IsCompleted: bool; …
//     public class Task<T>: Task public readonly property Result: T;
//     public class OwningTask<T>: Task where T: class
//
// ⇒ EVERY Task-family value is a `Task` (all three derive from it), so a
// Task-family operand answers `is Task` TRUE (and `is not Task` FALSE); a
// `Task<X>` answers `is Task<X>` TRUE and `is Task<Y>` (Y ≠ X) FALSE; and an
// `OwningTask<X>` is NOT a `Task<X>` (it derives from `Task`, not from
// `Task<X>`). §12.7's nullable-target rule ("`x is T?` has the answer of
// `x is T`") covers `is Task?`, and §12.7's null rule ("null is an instance
// of no type") makes a NULL task answer FALSE.
//
// The fix adds that static decision to `lowerTypeTest`, before the
// `!targetDesc` bail that drops the test.
//
// ── THE ORACLE ─────────────────────────────────────────────────────────────
//
// Every row asserts the EXACT stdout of a program that prints one bool — a
// POSITIVE claim about the program's observable behaviour. The pre-fix tree
// prints NOTHING for every one of these programs (the statement emits
// nothing), so every row here is RED without the fix, and no row can be
// satisfied by a program that merely compiles.

#include <string>

#include "support/aot_support.hpp"    // runAot — AOT test harness
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;

// The harness SKIPs (loudly, counted) when the toolchain is absent; a row
// that asserted through that would be asserting on nothing.
bool skipped(const JitResult& r) {
    return bezel::test::diagnosticMessageContains(r, "SKIP");
}

// A `Task<string>` producer — the issue's own repro shape
// (src/codegen/LlvmBackend_Async.cpp's async lowering mints the task).
constexpr const char* kTaskStringPrelude =
    "module M; import System; "
    "class C "
    "  public static async method S(): Task<string> return \"abc\"; end S; "
    "  public static method Main(): int ";

// `t is Task` over a `Task<string>` — §30.3: `Task<T>: Task` ⇒ TRUE.
constexpr const char* kIsTask =
    "    using var t := S(); "
    "    Console.WriteLine((t is Task).ToString()); "
    "    return 0; "
    "end Main; end C;";

// `t is Task<string>` over a `Task<string>` — the operand's own type ⇒ TRUE.
constexpr const char* kIsTaskString =
    "    using var t := S(); "
    "    Console.WriteLine((t is Task<string>).ToString()); "
    "    return 0; "
    "end Main; end C;";

// `t is Task<int>` over a `Task<string>` — a DIFFERENT instantiation ⇒ FALSE.
// This is the negative control: it pins that the fix does not answer TRUE for
// every target.
constexpr const char* kIsTaskInt =
    "    using var t := S(); "
    "    Console.WriteLine((t is Task<int>).ToString()); "
    "    return 0; "
    "end Main; end C;";

// `t is Task?` — §12.7: "A nullable target tests as its underlying type:
// `x is T?` has the answer of `x is T`" ⇒ TRUE.
constexpr const char* kIsNullableTask =
    "    using var t := S(); "
    "    Console.WriteLine((t is Task?).ToString()); "
    "    return 0; "
    "end Main; end C;";

// `t is not Task` — §12.7's negation of the `is Task` answer ⇒ FALSE.
constexpr const char* kIsNotTask =
    "    using var t := S(); "
    "    Console.WriteLine((t is not Task).ToString()); "
    "    return 0; "
    "end Main; end C;";

// A NULL task — §12.7: the answer is "`false` at `null`" ("a type test
// matching no absent value"), and §19.5 ("Type-pattern matching never matches
// `null`") states the same rule for the type-test family ⇒ FALSE.
constexpr const char* kIsTaskOverNull =
    "    var t: Task<string>? := null; "
    "    Console.WriteLine((t is Task).ToString()); "
    "    return 0; "
    "end Main; end C;";

std::string program(const char* body) {
    return std::string(kTaskStringPrelude) + body;
}

}  // namespace

// ── 1. `t is Task` over a `Task<string>` is TRUE ───────────────────────────
BEZEL_TEST(task_type_test_is_task_7870, is_task_over_a_task_string_is_true) {
    auto r = bezel::test::runAot(program(kIsTask));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

// ── 2. `t is Task<string>` over a `Task<string>` is TRUE ────────────────────
BEZEL_TEST(task_type_test_is_task_7870,
           is_task_string_over_a_task_string_is_true) {
    auto r = bezel::test::runAot(program(kIsTaskString));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

// ── 3. `t is Task<int>` over a `Task<string>` is FALSE (negative control) ───
BEZEL_TEST(task_type_test_is_task_7870,
           is_task_int_over_a_task_string_is_false) {
    auto r = bezel::test::runAot(program(kIsTaskInt));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// ── 4. `t is Task?` has the answer of `t is Task` (§12.7) ──────────────────
BEZEL_TEST(task_type_test_is_task_7870,
           is_nullable_task_has_the_same_answer_as_is_task) {
    auto r = bezel::test::runAot(program(kIsNullableTask));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

// ── 5. `t is not Task` is FALSE ────────────────────────────────────────────
BEZEL_TEST(task_type_test_is_task_7870, is_not_task_is_false) {
    auto r = bezel::test::runAot(program(kIsNotTask));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// ── 6. a NULL task matches no type (§12.7 / §19.5) ─────────────────────────
//
// §12.7: the answer is "`false` at `null`" — "a type test matching no absent
// value"; §19.5 ("Type-pattern matching never matches `null`") states the same
// rule for the type-test family.
BEZEL_TEST(task_type_test_is_task_7870, is_task_over_a_null_task_is_false) {
    auto r = bezel::test::runAot(program(kIsTaskOverNull));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}
