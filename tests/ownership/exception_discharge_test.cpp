// Spec-derived tests for Ownership slice O11-real — exception-path
// discharge over the real try/catch/finally exception edges (§28.3
// "Exception-path discharge"; §28.6 "Leak prevention").
//
// §28.3, verbatim: "Owned locals and owned parameters that are live when
// an exception propagates out of their enclosing block are disposed
// implicitly during stack unwinding. The compiler treats every owned
// local and owned parameter as if it were wrapped in an implicit
// try/finally whose finally disposes the value if it is still owned ... The
// discharge-requirement rule above only constrains *normal* exit: on a
// normal return or fall-through from the enclosing block, the owned local
// or parameter must reach a discharge point statically; on exception
// exits, the implicit auto-dispose runs."
//
// And the scoping: "When the exception passes through nested scopes, the
// inner scope's locals are disposed before the outer scope's." A value is
// disposed by the unwind only when the exception propagates OUT of *its*
// enclosing block — so a local declared in a try body is disposed when an
// exception escapes the try, but a local (or owned parameter) declared in
// the enclosing method scope is NOT disposed when an enclosing catch
// handles the exception, because the exception never leaves the method.
//
// These cases exercise the real F2 exception-edge CFG (the ownership pass
// now builds with treatTryAsOpaque=false). They cover both "now correctly
// accepted" (exception path auto-disposes → no leak) and "still correctly
// diagnosed" (the obligation survives on a normal-exit path → leak) cases.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Wrap a method body in a class with a few resolvable helper methods so the
// snippet exercises ownership, not unrelated name-resolution diagnostics.
// `Risky()` stands in for a throwable call inside a try body.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class T end T; "
        "class Error end Error; "
        "class C "
        "  public method Risky() end Risky; "
        "  public method Done() end Done; "
        "  public method Cond(): bool return true; end Cond; "
        "  public method M() ") + body + " end M; "
        "end C;";
}

// Same, but the method M takes the given parameter list.
std::string wrapParam(const std::string& params, const std::string& body) {
    return std::string(
        "module M; "
        "class T end T; "
        "class Error end Error; "
        "class C "
        "  public method Risky() end Risky; "
        "  public method Done() end Done; "
        "  public method M(") + params + ") " + body + " end M; "
        "end C;";
}

} // namespace

// ---- §28.3: owned acquired in a try then throw → auto-unwind, no leak ----

BEZEL_TEST(own_exc_disch, owned_in_try_then_throw_no_leak_28_3) {
    // `x` is acquired in the try body and the body throws. The exception
    // propagates out of the try body, so `x` is auto-disposed by the
    // implicit try/finally — no leak. (No normal exit from the try body
    // exists; the throw is the only way out.)
    auto r = parseAndCheckOwnership(wrap(
        "try var x := new T(); throw new Error(); "
        "catch var e: Error => this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_exc_disch, using_var_in_try_then_throw_no_leak_28_3) {
    // §28.3: on exception paths a `using var` and a plain `var` behave
    // identically — the implicit cleanup disposes the value.
    auto r = parseAndCheckOwnership(wrap(
        "try using var x := new T(); throw new Error(); "
        "catch var e: Error => this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.3: owned acquired, throw in a BRANCH of the try → no leak -------

BEZEL_TEST(own_exc_disch, owned_in_try_throw_in_branch_no_leak_28_3) {
    // On the throw branch `x` is auto-disposed by the unwind; on the
    // fall-through branch `x` reaches `x.Dispose()`. Both exit paths
    // discharge `x`, so no leak.
    auto r = parseAndCheckOwnership(wrap(
        "try var x := new T(); "
        "if this.Cond() then throw new Error(); end; "
        "x.Dispose(); "
        "catch var e: Error => this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.3: throwable call mid-try, value disposed before the throw ------

BEZEL_TEST(own_exc_disch, throwable_call_mid_try_then_dispose_no_leak_28_3) {
    // A throwable call (`Risky()`) runs while `x` is owned: on the exception
    // edge `x` is auto-disposed; on the normal continuation `x.Dispose()`
    // runs. No leak on either path.
    auto r = parseAndCheckOwnership(wrap(
        "try var x := new T(); this.Risky(); x.Dispose(); "
        "catch var e: Error => this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.6: owned in a try, undischarged on the NORMAL exit → leak -------

BEZEL_TEST(own_exc_disch, owned_in_try_undischarged_normal_path_leaks_28_6) {
    // §28.3 v0.150 (#1008 Part B): `x` is acquired in the try body and exits
    // normally undischarged. The throw edge already auto-disposed it; v0.150
    // applies the SAME implicit disposal to the normal-exit edge (auto-drop),
    // with the must-use warning — no leak, no error (verified ASan-clean on both
    // the normal and exception paths).
    auto r = parseAndCheckOwnership(wrap(
        "try var x := new T(); this.Risky(); "
        "catch var e: Error => this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

BEZEL_TEST(own_exc_disch, owned_in_try_finally_undischarged_normal_leaks_28_6) {
    // §28.3 v0.150: try…finally form — `x` auto-drops at the try-body scope end
    // on the normal-exit edge (after the finally), with the must-use warning;
    // no error. The auto-drop composes with the explicit `finally`.
    auto r = parseAndCheckOwnership(wrap(
        "try var x := new T(); this.Risky(); "
        "finally this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

// ---- §28.3 scoping: a catch re-acquires; discharge happens after --------

BEZEL_TEST(own_exc_disch, catch_reowns_then_discharged_after_no_leak_28_3) {
    // `x` is bound to an owned value on both the try path (then thrown out
    // of, auto-disposed) and re-acquired in the catch; after the try it is
    // explicitly disposed. No leak.
    auto r = parseAndCheckOwnership(wrap(
        "var x: T; "
        "try x := new T(); throw new Error(); "
        "catch var e: Error => x := new T(); end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.3 scoping: an ENCLOSING-scope owned local is NOT auto-disposed --
//      when an in-method catch handles the exception.

BEZEL_TEST(own_exc_disch, enclosing_owned_caught_undischarged_leaks_28_6) {
    // §28.3 v0.150: `x` is declared in the method scope BEFORE the try. The catch
    // handles the exception, so the method exits NORMALLY — `x` auto-drops at
    // method-scope end on the normal-exit edge (must-use warning), no error.
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); "
        "try this.Risky(); catch var e: Error => this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

BEZEL_TEST(own_exc_disch, enclosing_owned_caught_discharged_after_no_leak_28_3) {
    // Same enclosing-scope `x`, but discharged after the try on the normal
    // continuation — clean.
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); "
        "try this.Risky(); catch var e: Error => this.Done(); end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.3 scoping: owned PARAMETER and exception paths -----------------

BEZEL_TEST(own_exc_disch, owned_param_caught_undischarged_auto_drops_28_3) {
    // §28.3 v0.152: an owned parameter's scope is the method body, which encloses
    // the try. The catch handles the exception, so the method exits NORMALLY — the
    // parameter auto-drops at method-scope end on the normal-exit edge (must-use
    // warning), no error. (Pre-v0.152 this was an undischarged-param leak/reject.)
    auto r = parseAndCheckOwnership(wrapParam(
        "owned p: T",
        "try this.Risky(); catch var e: Error => this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "p"));
}

BEZEL_TEST(own_exc_disch, owned_param_naked_throw_no_leak_28_3) {
    // A naked throw with no enclosing handler propagates out of the method:
    // the whole method unwinds, so the owned parameter IS auto-disposed by
    // the implicit try/finally — no leak.
    auto r = parseAndCheckOwnership(wrapParam(
        "owned p: T", "throw new Error();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.3: nested try — inner throw auto-disposes the inner body local --

BEZEL_TEST(own_exc_disch, nested_try_inner_owned_throw_no_leak_28_3) {
    // The inner try body acquires `x` and throws; `x` is auto-disposed as
    // the exception escapes the inner try body. The outer catch is reached
    // by re-throws; no owned local survives to a normal-exit edge → no leak.
    auto r = parseAndCheckOwnership(wrap(
        "try "
        "  try var x := new T(); throw new Error(); "
        "  catch var e: Error => this.Done(); end; "
        "catch var e2: Error => this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.3 + loops (termination over the exception-edge CFG) -------------

BEZEL_TEST(own_exc_disch, loop_in_try_dispose_each_iter_no_leak_28_3) {
    // A loop inside a try, with a throwable call mid-body, disposing the
    // owned local each iteration: clean. Also exercises fixpoint
    // convergence over the loop back-edge crossed with the try exception
    // edges (the finally/catch join is reached from many predecessors).
    auto r = parseAndCheckOwnership(wrap(
        "try while this.Cond() do "
        "  var x := new T(); this.Risky(); x.Dispose(); "
        "end; catch var e: Error => this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_exc_disch, loop_in_try_undischarged_each_iter_leaks_28_6) {
    // §28.3 v0.150: the loop-body owned local auto-drops at the end of each
    // iteration's scope (must-use warning), on both the per-iteration normal
    // edge and the exception edge — no error, no leak (ASan-verified per-iter).
    auto r = parseAndCheckOwnership(wrap(
        "try while this.Cond() do "
        "  var x := new T(); this.Risky(); "
        "end; catch var e: Error => this.Done(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}
