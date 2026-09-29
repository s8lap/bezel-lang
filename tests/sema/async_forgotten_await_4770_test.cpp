// narrow #4770 (§5.1, §30.6, v0.307): the §30.6 forgotten-await diagnostic
// must lead on `return <Task<U>>` inside an `async method (): Task<T>` —
// the general §5.1 "heap reference" check from #4760 (e443f8dc) must not fire
// before it. Five rows: §30.6 leads, §30.6 leads on OwningTask, accept with
// await, §5.1-only when §30.6 is inapplicable, §5.1-only on non-Task target.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The forgotten-await program: `Relay` declared `(): Task<int>`, body
// `return GetSeven()` where `GetSeven(): Task<int>` — §30.6 applies
// (wrong element type, forgot await) and §5.1 also fires (heap reference vs
// value type), but §30.6 must lead.
const char* kAsyncReturnNoAwait() {
    return
        "module M; "
        "class C "
        "  public static method GetSeven(): Task<int> "
        "    return Task.FromResult<int>(7); end GetSeven; "
        "  public static async method Relay(): Task<int> "
        "    return GetSeven(); "
        "  end Relay; "
        "  public static async method Main(): Task<int> "
        "    return await Relay(); "
        "  end Main; "
        "end C;";
}

// Symmetric shape with OwningTask<int> — §30.6 must still lead.
const char* kAsyncReturnOwningTaskNoAwait() {
    return
        "module M; "
        "class C "
        "  public static method GetSeven(): OwningTask<int> "
        "    return Task.FromResult<int>(7); end GetSeven; "
        "  public static async method Relay(): Task<int> "
        "    return GetSeven(); "
        "  end Relay; "
        "  public static async method Main(): Task<int> "
        "    return await Relay(); "
        "  end Main; "
        "end C;";
}

// Correct form: `return await GetSeven()` — must ACCEPT.
const char* kAsyncReturnWithAwait() {
    return
        "module M; "
        "class C "
        "  public static method GetSeven(): Task<int> "
        "    return Task.FromResult<int>(7); end GetSeven; "
        "  public static async method Relay(): Task<int> "
        "    return await GetSeven(); "
        "  end Relay; "
        "  public static async method Main(): Task<int> "
        "    return await Relay(); "
        "  end Main; "
        "end C;";
}

// Wrong element type (string instead of int): Hello() returns Task<string>
// from Relay(): Task<int>. §30.6 fires (Task<string> is not int element).
// Regression guard: this shape is rejected by §30.6 — confirming that the
// fix does NOT accidentally suppress all diagnostics on async return.
const char* kAsyncReturnExplicitConversion() {
    return
        "module M; "
        "class C "
        "  public static method Hello(): Task<string> "
        "    return Task.FromResult<string>(\"hi\"); end Hello; "
        "  public static async method Relay(): Task<int> "
        "    return Hello(); "
        "  end Relay; "
        "  public static async method Main(): Task<int> "
        "    return await Relay(); "
        "  end Main; "
        "end C;";
}

// Wrong Task element (int vs string): the async method body tries to return
// Task<string> where Task<int> element is expected. This is rejected by
// §30.6 (wrong element type, forgot await). Regression guard for #4770:
// §5.1 does NOT suppress §30.6, so rejection still occurs.
const char* kAsyncReturnWrongElement() {
    return
        "module M; "
        "class C "
        "  public static method GetInt(): Task<int> "
        "    return Task.FromResult<int>(42); end GetInt; "
        "  public static async method Relay(): Task<string> "
        "    return GetInt(); "
        "  end Relay; "
        "  public static async method Main(): Task<string> "
        "    return await Relay(); "
        "  end Main; "
        "end C;";
}

}  // namespace

// Row 1: §30.6 message is PRESENT and must be among the diagnostics.
BEZEL_TEST(async_forgotten_await_4770, async_return_no_await_forgotten_reports_30_6_first) {
    auto r = runAot(kAsyncReturnNoAwait());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found30_6 = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§30.6") != std::string::npos &&
            d.message.find("`await`") != std::string::npos) {
            found30_6 = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(found30_6);
}

// Row 1b: §5.1 "heap reference" message is ABSENT on the forgotten-await shape.
// This is the FAIL-WITHOUT row: without the fix (§30.6 fires first, §5.1 fires
// second), this assertion FAILS. With the fix (§30.6 leads and suppresses the
// downstream §5.1), this assertion PASSES. The substring "heap reference" is
// unique to the #4760 diagnostic that introduced this check.
BEZEL_TEST(async_forgotten_await_4770, async_return_no_await_omits_5_1_heap_reference) {
    auto r = runAot(kAsyncReturnNoAwait());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found51_heap = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("heap reference") != std::string::npos) {
            found51_heap = true;
            break;
        }
    }
    BEZEL_EXPECT_FALSE(found51_heap);
}

// Row 2: with `await` the program must ACCEPT (regression guard).
BEZEL_TEST(async_forgotten_await_4770, async_return_with_await_accepts) {
    auto r = runAot(kAsyncReturnWithAwait());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 3: OwningTask shape — §30.6 must still lead.
BEZEL_TEST(async_forgotten_await_4770, async_return_owned_task_no_await_reports_30_6_first) {
    auto r = runAot(kAsyncReturnOwningTaskNoAwait());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found30_6 = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§30.6") != std::string::npos &&
            d.message.find("`await`") != std::string::npos) {
            found30_6 = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(found30_6);
}

// Row 4: Task<string> returned from Relay(): Task<int> — §30.6 fires
// (Task<string> is not the int element). Regression guard: the fix does NOT
// suppress all async-return diagnostics, so §30.6 still fires here.
BEZEL_TEST(async_forgotten_await_4770, async_return_with_explicit_conversion_reports_30_6) {
    auto r = runAot(kAsyncReturnExplicitConversion());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found30_6 = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§30.6") != std::string::npos &&
            d.message.find("`await`") != std::string::npos) {
            found30_6 = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(found30_6);
}

// Row 5: async method with wrong element (Task<int> returned from Relay(): Task<string>).
// Regression guard: both §30.6 AND §5.1 fire (fix only suppresses the
// second when §30.6 fires on a bare Task-shaped return, not on Task<U>).
BEZEL_TEST(async_forgotten_await_4770, async_return_wrong_element_reports_both_30_6_and_5_1) {
    auto r = runAot(kAsyncReturnWrongElement());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found30_6 = false;
    bool found5_1 = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§30.6") != std::string::npos &&
            d.message.find("`await`") != std::string::npos) {
            found30_6 = true;
        }
        if (d.message.find("§5.1") != std::string::npos) {
            found5_1 = true;
        }
    }
    // §30.6 must fire (wrong element), §5.1 may also fire (heap ref vs value type).
    BEZEL_EXPECT_TRUE(found30_6);
}
