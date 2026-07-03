// Ownership tests for TaskScope's CONSUMPTION rules — stdlib §6.10 (v0.175,
// #808).
//
// §6.10: "Consumption is `await using` only — a plain `using var`, or
// dropping the scope undisposed, is a compile error naming this rule: a
// synchronous destructor cannot await the drain, and live children hold
// scope-bounded borrows (§28.14), so no synchronous escape is sound."
// "`await Join()` is mandatory on the normal path (compile error where
// provable, InvalidOperationException from DisposeAsync otherwise)."
//
// The provable subset implemented: no `await L.Join()` anywhere AND `L` never
// delegated (never passed as a bare argument / captured by a closure) — then
// the missing join is a deterministic compile error; otherwise the runtime
// IOE from DisposeAsync backstops (tested in tests/codegen).

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

std::string asyncMain(const std::string& body) {
    return std::string(
        "module M; import System.Threading; "
        "class Prog ") +
        "  public static async method Helper(s: TaskScope): Task "
        "    await s.Join(); "
        "  end Helper; "
        "  public static async method Main(): Task<int> " + body +
        "    return 0; "
        "  end Main; "
        "end Prog;";
}

}  // namespace

// §6.10: the canonical shape — await using + await Join — is clean.
BEZEL_TEST(taskscope_consumption_610, await_using_with_join_accepted) {
    auto r = parseAndCheckOwnership(asyncMain(
        "    await using var scope := new TaskScope(); "
        "    await scope.Join(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.10: a plain (synchronous) `using var` TaskScope is rejected naming the
// await-using-only rule.
BEZEL_TEST(taskscope_consumption_610, plain_using_var_rejected) {
    auto r = parseAndCheckOwnership(asyncMain(
        "    using var scope := new TaskScope(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "await using"));
}

// §6.10: a plain `var` TaskScope (an undisposed / synchronously-droppable
// binding) is rejected naming the rule.
BEZEL_TEST(taskscope_consumption_610, plain_var_rejected) {
    auto r = parseAndCheckOwnership(asyncMain(
        "    var scope := new TaskScope(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "await using"));
}

// §6.10: discarding a TaskScope (`_ := new TaskScope()`) is rejected.
BEZEL_TEST(taskscope_consumption_610, discard_rejected) {
    auto r = parseAndCheckOwnership(asyncMain(
        "    _ := new TaskScope(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "await using"));
}

// §6.10: an explicit synchronous `.Dispose()` on a TaskScope is rejected —
// it cannot await the drain.
BEZEL_TEST(taskscope_consumption_610, explicit_dispose_rejected) {
    auto r = parseAndCheckOwnership(asyncMain(
        "    await using var scope := new TaskScope(); "
        "    await scope.Join(); "
        "    scope.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "synchronously"));
}

// §6.10: missing `await Join()` with NO delegation is the PROVABLE case — a
// compile error naming the rule.
BEZEL_TEST(taskscope_consumption_610, missing_join_provable_rejected) {
    auto r = parseAndCheckOwnership(asyncMain(
        "    await using var scope := new TaskScope(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "Join()"));
}

// §6.10: a DELEGATED scope (passed as an argument — the callee may join it)
// is NOT provably missing the join; no compile error (the runtime IOE from
// DisposeAsync is the backstop).
BEZEL_TEST(taskscope_consumption_610, missing_join_with_delegation_accepted) {
    auto r = parseAndCheckOwnership(asyncMain(
        "    await using var scope := new TaskScope(); "
        "    await Prog.Helper(scope); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.10 + §28.14: TaskScope.Spawn's parameter is `scoped` — a capture-bearing
// closure is ADMITTED against the await-using scope local when its sources
// are declared before the scope…
BEZEL_TEST(taskscope_consumption_610, spawn_capture_sources_before_admitted) {
    auto r = parseAndCheckOwnership(asyncMain(
        "    var count := 0; "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async () => do count := count + 1; end); "
        "    await scope.Join(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// …and REJECTED when a reference-typed source is declared after the scope
// (§28.14 declaration order applied to the builtin Spawn).
BEZEL_TEST(taskscope_consumption_610, spawn_capture_source_after_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class Res "
        "  public method Ping() end Ping; "
        "  destructor end; "
        "end Res; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    await using var scope := new TaskScope(); "
        "    using var res := new Res(); "
        "    scope.Spawn(async () => do res.Ping(); end); "
        "    await scope.Join(); "
        "    return 0; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "declared before"));
}
