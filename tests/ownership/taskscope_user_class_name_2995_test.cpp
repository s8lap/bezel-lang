// fix #2995 (sema, spec §11.3 / stdlib §6.10, v0.240): the front-end's
// TaskScope consumption predicate (`typeRefIsTaskScope` /
// `exprTypeIsTaskScope` in `MethodOwnership`) used to compare the bare name
// against "TaskScope", so a USER class that happened to be named `TaskScope`
// (legal under §11.3 — no names are reserved) was rejected with stdlib
// §6.10's two TaskScope-specific diagnostics on a program that contained
// no TaskScope built-in:
//
//   * `using var s := new TaskScope();`     -> "may only be consumed by
//                                              `await using`"
//   * `await using var s := new TaskScope();` -> "await s.Join() is missing"
//
// and no spelling admitted the class. The fix mirrors #2997 (which closed
// the same shape in `Emitter::builtinTypeName` for the codegen chain): a
// synthetic builtin TypeSymbol leaves `TypeSymbol::moduleName` empty, while
// a local user class (SymbolCollection.cpp:265) and an imported one
// (ModuleLoader) populate it. The predicate now requires the name match AND
// the empty moduleName, so a user class never satisfies it.
//
// The regression test below exercises the predicate's every branch on a
// user class, and confirms the builtin TaskScope still gets the
// diagnostics the predicate emits.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// True iff the diagnostics for `r` include the §6.10 "may only be consumed
// by `await using`" wording.
bool diagSaysTaskScopeRequiresAwaitUsing(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("may only be consumed by `await using`") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

// True iff the diagnostics for `r` include the §6.10 "await Join() is
// missing" wording.
bool diagSaysAwaitJoinMissing(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`await") != std::string::npos &&
            d.message.find(".Join()` is missing") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// A reusable user class `TaskScope` for the §11.3 / stdlib §6.10 (v0.240)
// "no names are reserved" check. The diagnostic's "your `s` is a TaskScope"
// text is computed off the resolved TypeRef, not this name — fixing the
// predicate to exclude the user class by its declared moduleName is what
// suppresses it. Declared at module scope (not inside the method), since
// nested type declarations inside a method body are §11.3's "local type"
// position; a module-level class is the more obvious spelling.
constexpr const char* kUserTaskScopeDecl =
    "class Payload "
    "  public readonly property N: int := 7; "
    "end Payload; "
    "class TaskScope "
    "  private owned _p: Payload := new Payload(); "
    "  public readonly property Tag: int := 3; "
    "end TaskScope; ";

std::string wrapWithUserTaskScope(const std::string& body) {
    return std::string("module M; ") + kUserTaskScopeDecl +
        "class C "
        "  public static method Main(): int " +
        body +
        "    return s.Tag; "
        "  end Main; "
        "end C;";
}

}  // namespace

// §11.3 / stdlib §6.10 (v0.240): a USER class named `TaskScope` bound to
// `using var` is a legal program — the predicate must NOT raise the §6.10
// "may only be consumed by `await using`" diagnostic on a type that is not
// the §6.10 builtin. The fix mirrors #2997's `moduleName.empty()`
// discriminator.
BEZEL_TEST(taskscope_user_class_name_2995, using_var_accepted) {
    auto r = parseAndCheckOwnership(wrapWithUserTaskScope(
        "using var s := new TaskScope(); "));
    BEZEL_EXPECT_FALSE(diagSaysTaskScopeRequiresAwaitUsing(r));
}

// §11.3: a USER class named `TaskScope` bound to a plain `var` triggers
// the §28.3 auto-drop warning (the local `s` has an owned field), but the
// §6.10 "may only be consumed by `await using`" wording must not fire on
// a user class.
BEZEL_TEST(taskscope_user_class_name_2995, plain_var_no_taskscope_diag) {
    auto r = parseAndCheckOwnership(wrapWithUserTaskScope(
        "var s := new TaskScope(); "));
    BEZEL_EXPECT_FALSE(diagSaysTaskScopeRequiresAwaitUsing(r));
}

// §6.10 preservation: the BUILT-IN TaskScope (a class from
// System.Threading) is still rejected with the §6.10 diagnostics. This
// guards against the fix widening into a no-op for the real builtin — the
// predicate must keep firing on the real builtin while now rejecting the
// user class.
BEZEL_TEST(taskscope_user_class_name_2995, builtin_using_var_still_rejected) {
    auto r = parseAndCheckOwnership(std::string(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    using var scope := new TaskScope(); "
        "    return 0; "
        "  end Main; "
        "end Prog;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagSaysTaskScopeRequiresAwaitUsing(r));
}

// §6.10 preservation: the BUILT-IN TaskScope with `await using var` and
// no `await Join()` is still rejected.
BEZEL_TEST(taskscope_user_class_name_2995, builtin_await_using_no_join_still_rejected) {
    auto r = parseAndCheckOwnership(std::string(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    await using var scope := new TaskScope(); "
        "    return 0; "
        "  end Main; "
        "end Prog;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagSaysAwaitJoinMissing(r));
}

// §6.10 preservation: the BUILT-IN TaskScope with `await using var` AND
// `await Join()` is still accepted (the canonical shape).
BEZEL_TEST(taskscope_user_class_name_2995, builtin_await_using_with_join_accepted) {
    auto r = parseAndCheckOwnership(std::string(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Main(): Task<int> "
        "    await using var scope := new TaskScope(); "
        "    await scope.Join(); "
        "    return 0; "
        "  end Main; "
        "end Prog;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
