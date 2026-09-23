// Spec-derived regression test for rule-corpus `s35_4_0006` (spec §30.1 /
// §30.5 / §35.4, v0.307).
//
// THE GOVERNING RULE. Spec §30.1 "`async` methods" (v0.307): "`Task<T>` for
// some type T admitted by §30.5's producer taxonomy (a move-only value T is
// admitted by neither container)." Spec §35.4 "The classification" (v0.307):
// "No §30.5 producer row admits a move-only value result, so a `Task<T>` or
// `OwningTask<T>` declared over an owning choice is a compile error at the
// declaration."
//
// ROOT CAUSE. `ExprTC::asyncReturnCtxAndType` (src/sema/ExprTypecheck.cpp) —
// the ONE place that peels an async method's (or async lambda's) declared
// `Task<T>`/`OwningTask<T>` return type down to its element T — classified
// the peeled element as the body's ordinary ValueMethod result type with NO
// check that T is a §15.3 move-only kind (an owning inline array or an
// owning-classified choice). The existing async-signature borrow check
// (`MethodOwnership::checkAsyncSignatureBorrowBearing`, Ownership_Checks.cpp)
// only rejects a BorrowBearing-classified choice result — an Owning choice
// (rule 1 of §35.4's classification takes precedence over rule 2) never
// reaches it, so a plain owning choice slipped through both checks entirely.
//
// MEASURED on `bf7a7515` (this lane's base, `r19-c20`) with `build/bezelc`:
// `public static async method Load(): Task<LoadOutcome>` (`LoadOutcome` an
// owning choice) compiled with rc=0.
//
// THE FIX. `asyncReturnCtxAndType`'s `Task<T>`/`OwningTask<T>` arm now calls
// `common::isMoveOnlyValueType` on the peeled element and rejects at the
// declared-return-type's own span when it is a move-only kind — the single
// site both an async METHOD's declared return type and an async LAMBDA's
// ascribed return type pass through (both callers of this function), so the
// rejection covers both declaration forms.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

}  // namespace

// rule-corpus `s35_4_0006`, verbatim: an async method's declared return type
// is `Task<T>` over an OWNING choice.
BEZEL_TEST(sema_task_over_owning_choice_rejected_s35_4_0006,
           async_method_task_over_owning_choice_rejected) {
    const char* src =
        "module Conf; "
        "public class Widget "
        "  public constructor() "
        "  end constructor; "
        "end Widget; "
        "public choice LoadOutcome "
        "  Loaded(owned w: Widget), "
        "  Failed; "
        "end LoadOutcome; "
        "public class Program "
        "  public static async method Load(): Task<LoadOutcome> "
        "    return LoadOutcome.Failed; "
        "  end Load; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "move-only"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""35.4"));
}

// Companion: the async-LAMBDA ascribed-return spelling reaches the SAME
// `asyncReturnCtxAndType` authority and must be rejected identically.
BEZEL_TEST(sema_task_over_owning_choice_rejected_s35_4_0006,
           async_lambda_task_over_owning_choice_rejected) {
    const char* src =
        "module Conf; "
        "public class Widget "
        "  public constructor() "
        "  end constructor; "
        "end Widget; "
        "public choice LoadOutcome "
        "  Loaded(owned w: Widget), "
        "  Failed; "
        "end LoadOutcome; "
        "public class Program "
        "  public static method Main() "
        "    var f: () -> Task<LoadOutcome> := async lambda(): Task<LoadOutcome> => do "
        "      return LoadOutcome.Failed; "
        "    end; "
        "    _ := f; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "move-only"));
}

// ACCEPT control: `Task<T>` over an ORDINARY (value-like) choice — no
// move-only payload at all — must be entirely unaffected.
BEZEL_TEST(sema_task_over_owning_choice_rejected_s35_4_0006,
           task_over_value_like_choice_still_accepted) {
    BEZEL_EXPECT_TRUE(runs(1,
        "module Conf; "
        "public choice Status "
        "  Ready(code: int), "
        "  Empty; "
        "end Status; "
        "public class Program "
        "  public static async method Load(): Task<Status> "
        "    return Status.Ready(1); "
        "  end Load; "
        "  public static method Main(): int "
        "    using var task := Load(); "
        "    var result := (case task.Result of "
        "      Ready(var c) => c, "
        "      Empty        => 0 "
        "    ); "
        "    return result; "
        "  end Main; "
        "end Program;"));
}

// ACCEPT control: `Task<T>` over an ordinary class result — the common case
// — must be entirely unaffected by the new move-only gate.
BEZEL_TEST(sema_task_over_owning_choice_rejected_s35_4_0006,
           task_over_class_result_still_accepted) {
    BEZEL_EXPECT_TRUE(runs(7,
        "module Conf; "
        "public class Program "
        "  public static async method GetSeven(): Task<int> "
        "    return 7; "
        "  end GetSeven; "
        "  public static method Main(): int "
        "    using var t := GetSeven(); "
        "    return t.Result; "
        "  end Main; "
        "end Program;"));
}
