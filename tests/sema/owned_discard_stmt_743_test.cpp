// Spec-derived sema tests for fix #743 — discarding a freshly OWNED value that
// requires disposal as a bare expression statement is a §21.9 hard error.
//
// Authority: §21.9 — "For non-trivial-dispose class types and for Task, the
// warning is replaced by a hard error directing the caller to
// `using var _ := expression;`, an explicit `.Dispose()`, or
// `Task.FireAndForget` as appropriate."
//
// The check is gated on the value being owned-PRODUCED at the statement (a
// `new T()` or a call whose method returns `owned T`), so the ubiquitous
// borrowed-return statement `obj.Foo();` is never false-rejected.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiscardLeakError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("21.9") != std::string::npos &&
            d.message.find("leaks the resource") != std::string::npos)
            return true;
    return false;
}

// #1014: the §17.13 task-discard error is recognized by its Task.FireAndForget
// direction ("abandons a running operation" + names FireAndForget).
bool hasTaskDiscardError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("abandons a running") != std::string::npos &&
            d.message.find("FireAndForget") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §21.9: `new T();` where T is non-trivial-dispose (a destructor) — hard error.
BEZEL_TEST(owned_discard_743, new_non_trivial_dispose_is_error) {
    auto r = parseSemaAll(
        "module M; "
        "class R public _h: int := 0; destructor _h := 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  new R(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasDiscardLeakError(r));
}

// §21.9: a call returning `owned T` (T non-trivial-dispose) discarded — error.
BEZEL_TEST(owned_discard_743, owned_returning_call_is_error) {
    auto r = parseSemaAll(
        "module M; "
        "class R public _h: int := 0; destructor _h := 1; end destructor; end R; "
        "class F public static method Make(): owned R return new R(); end Make; end F; "
        "class C public static method Main(): int "
        "  F.Make(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasDiscardLeakError(r));
}

// §21.9: a BORROWED-return statement is NOT rejected — the caller owns nothing.
BEZEL_TEST(owned_discard_743, borrowed_return_statement_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class R public _h: int := 0; destructor _h := 1; end destructor; "
        "  public method Touch(): int return _h; end Touch; end R; "
        "class C public static method Main(): int "
        "  using var r := new R(); "
        "  r.Touch(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasDiscardLeakError(r));
}

// §21.9: `new T();` for a TRIVIAL-dispose class (no destructor) — accepted.
BEZEL_TEST(owned_discard_743, new_trivial_dispose_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Plain public _n: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  new Plain(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasDiscardLeakError(r));
}

// §21.9: a void-returning method call statement is unaffected.
BEZEL_TEST(owned_discard_743, void_call_statement_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Noop() end Noop; "
        "  public static method Main(): int "
        "    Noop(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiscardLeakError(r));
}

// ===========================================================================
// §21.9 / §17.13 / §2.11 (#949): `<event>.Subscribe(...)` returns an owned
// Subscription (non-trivial-dispose). Dropping it as a statement or discarding
// it with `_ :=` is a hard error; binding it (`using var`) or disposing it is
// fine. The Subscribe call result is untyped, so it is recognized structurally.
// ===========================================================================

namespace {
const char* kButton =
    "module M; "
    "public class Button "
    "  public event Clicked: int; "
    "  public method Click(v: int) Clicked.Raise(v); end Click; "
    "end Button; ";
}  // namespace

// REJECT: bare-statement drop of a Subscribe result (§21.9).
BEZEL_TEST(owned_discard_743, subscribe_statement_drop_is_error_949) {
    auto r = parseSemaAll(
        std::string(kButton) +
        "public class C public static method Main(): int "
        "  using var btn: Button := new Button(); "
        "  btn.Clicked.Subscribe((v) => do end); "  // dropped Subscription
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiscardLeakError(r));
}

// REJECT: `_ :=` discard of a Subscribe result (§17.13).
BEZEL_TEST(owned_discard_743, subscribe_underscore_discard_is_error_949) {
    auto r = parseSemaAll(
        std::string(kButton) +
        "public class C public static method Main(): int "
        "  using var btn: Button := new Button(); "
        "  _ := btn.Clicked.Subscribe((v) => do end); "  // discarded Subscription
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ACCEPT: `using var` binding of a Subscribe result.
BEZEL_TEST(owned_discard_743, subscribe_using_var_ok_949) {
    auto r = parseSemaAll(
        std::string(kButton) +
        "public class C public static method Main(): int "
        "  using var btn: Button := new Button(); "
        "  using var s := btn.Clicked.Subscribe((v) => do end); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: explicit `.Dispose()` of a Subscribe result.
BEZEL_TEST(owned_discard_743, subscribe_explicit_dispose_ok_949) {
    auto r = parseSemaAll(
        std::string(kButton) +
        "public class C public static method Main(): int "
        "  using var btn: Button := new Button(); "
        "  var s := btn.Clicked.Subscribe((v) => do end); s.Dispose(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §17.13 / §30.10 (#1014): discarding a `Task` / `Task<T>` / `OwnedTask<T>` is a
// compile error directing the caller to `Task.FireAndForget` — disposing a task
// abandons a running operation. `await`ing a task (its result type) and trivial
// values are unaffected.
// ===========================================================================

// REJECT: `_ := <Task<T>>` discard of an async call result.
BEZEL_TEST(owned_discard_743, task_generic_discard_is_error_1014) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Work(): Task<int> return 0; end Work; "
        "  public static method Main(): int "
        "    _ := Work(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTaskDiscardError(r));
}

// REJECT: `_ := <Task>` discard of a void async call (plain Task, no result).
BEZEL_TEST(owned_discard_743, task_void_discard_is_error_1014) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Work(): Task return; end Work; "
        "  public static method Main(): int "
        "    _ := Work(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTaskDiscardError(r));
}

// REJECT: `_ := <OwnedTask<T>>` discard (Pattern B; still a task, FireAndForget).
BEZEL_TEST(owned_discard_743, owned_task_discard_is_error_1014) {
    auto r = parseSemaAll(
        "module M; "
        "class R public _h: int := 0; destructor _h := 1; end destructor; end R; "
        "class C "
        "  public static async method Make(): OwnedTask<R> return new R(); end Make; "
        "  public static method Main(): int "
        "    _ := Make(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTaskDiscardError(r));
}

// ACCEPT: `_ := await <Task<T>>` — await yields the RESULT type (`int`), not a
// task; discarding a trivial result is fine and must NOT trip the task reject.
BEZEL_TEST(owned_discard_743, awaited_task_result_discard_ok_1014) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Work(): Task<int> return 0; end Work; "
        "  public static async method Caller(): Task "
        "    _ := await Work(); "
        "    return; "
        "  end Caller; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasTaskDiscardError(r));
}

// ACCEPT: a trivial-value discard is unaffected by the task reject.
BEZEL_TEST(owned_discard_743, trivial_discard_not_task_error_1014) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  _ := 5; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasTaskDiscardError(r));
}
