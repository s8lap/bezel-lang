// Regression tests for issue #9084 — §28.14 *The call-bounded default*
// (v0.359) at an INDEXER ACCESSOR body.
//
// THE DEFECT.  §28.14's callee-side verification is the call-bounded scan
// seeded from a body's OWN plain function-typed formals.  `MethodOwnership::run`
// seeds a METHOD body's formals; an indexer accessor body was never seeded: a
// block getter reaches `run` only when its body is proved confined (#8767), and
// then with its diagnostics suppressed, and an expression-bodied getter, the
// indexer's own `=> E` body and an expression-bodied setter (`set => call;`)
// reach it not at all.  So an indexer could store, return or forward into a
// `scoped` position its own call-bounded parameter, which the METHOD twin of
// the same body is rejected for.  (A setter's BLOCK body — both `set`
// spellings that parse as a statement — runs the full pass since #9073 and is
// already rejected.)
//
// THE SPEC.  §28.14 (v0.359), *The call-bounded default*: "A function-typed
// parameter declared *without* `scoped` is **call-bounded** … Within the callee
// the callback may be invoked, bound to locals, and passed on as an argument
// to another call-bounded parameter … It may not be stored in a field of any
// object …, stored into any collection, a local one included …, returned, …
// or forwarded into a `scoped` parameter position: the callee cannot see its
// argument's capture set, so it cannot vouch for the §28.14 admission … These
// obligations are compiler-verified for user-authored callees".
//
// ROWS.  One reject row per (sink × accessor body form) that is RED on the
// base tree (the program compiles there), each asserting the METHOD twin's own
// diagnostic.  Two admit controls: the uses §28.14 names as legal.
// ORACLE.  `runAot`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const char* kCallBounded =
    "parameter `q` is call-bounded; declare it `scoped` to store it";
const char* kScopedForward =
    "parameter `q` is call-bounded and may not be forwarded into a `scoped` "
    "parameter position";

void expectCallBoundedReject(const JitResult& r, const char* sink) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCallBounded));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, sink));
}

void expectScopedForwardReject(const JitResult& r) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kScopedForward));
}

// `members` declares the indexer on `Fl`; `useStmt` is the body of
// `Mk(r: Res): int`, which reads (or writes) the indexer with a closure that
// captures the caller's local `r`.  `Main` calls it 50 times and prints the
// sum.
std::string indexerProgram(const std::string& members,
                           const std::string& useStmt) {
    return "module M; "
           "import System; "
           "public class Res "
           "  public V: int; "
           "  public constructor() V := 42; end constructor; "
           "end Res; "
           "public class Sink "
           "  private scoped _cb: (() -> int)? := null; "
           "  public constructor() end constructor; "
           "  public method Take(scoped cb: () -> int): int "
           "    _cb := cb; return 0; "
           "  end Take; "
           "  public static method Plain(f: () -> int): int return f(); end Plain; "
           "end Sink; "
           "public class Fl2 "
           "  public constructor() end constructor; "
           "  public property [q: () -> int]: int => q(); "
           "end Fl2; "
           "public class Fl "
           "  private _q: (() -> int)? := null; "
           "  private _l: Array<() -> int> := []; "
           "  " +
           members +
           "  public constructor() end constructor; "
           "end Fl; "
           "public class Program "
           "  public static method Mk(fl: Fl, r: Res): int " +
           useStmt +
           "  end Mk; "
           "  public static method Main(): int "
           "    var fl := new Fl(); "
           "    var t := 0; "
           "    for var i in 0 ..< 50 do "
           "      var r := new Res(); "
           "      t := t + Mk(fl, r); "
           "    end for; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

const char* kGetUse = "return fl[lambda() => r.V];";
const char* kSetUse = "fl[lambda() => r.V] := 1; return 0;";
const char* kRetUse = "var g := fl[lambda() => r.V]; return g();";

}  // namespace

// ---- STORE: "It may not be stored in a field of any object …, stored into
// any collection" -------------------------------------------------------------

// REJECT — #9084's STORE repro: a BLOCK getter stores its own call-bounded
// parameter in a field.  RED on the base tree.
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           block_getter_stores_call_bounded_index_param_in_field_rejected_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: int "
        "  get => do _q := q; return q(); end; "
        "end; ",
        kGetUse));
    expectCallBoundedReject(r, "stored in a field of any object");
}

// REJECT — the collection twin (the reviewer's U2 shape): a BLOCK getter adds
// its own call-bounded parameter to a field's collection.  RED on the base.
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           block_getter_stores_call_bounded_index_param_into_collection_rejected_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: int "
        "  get => do this._l.Add(q); return 0; end; "
        "end; ",
        kGetUse));
    expectCallBoundedReject(r,
                            "stored into a collection reachable beyond the call");
}

// ---- SCOPED-FORWARD: "or forwarded into a `scoped` parameter position" -------

// REJECT — #9084's SCOPED-FORWARD repro: the indexer's own `=> E` body.
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           indexer_expression_body_forwards_call_bounded_index_param_to_scoped_rejected_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: int => (new Sink()).Take(q); ",
        kGetUse));
    expectScopedForwardReject(r);
}

// REJECT — the `get => E` accessor form.
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           expression_getter_forwards_call_bounded_index_param_to_scoped_rejected_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: int "
        "  get => (new Sink()).Take(q); "
        "end; ",
        kGetUse));
    expectScopedForwardReject(r);
}

// REJECT — the `get => do … end` accessor form.
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           block_getter_forwards_call_bounded_index_param_to_scoped_rejected_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: int "
        "  get => do return (new Sink()).Take(q); end; "
        "end; ",
        kGetUse));
    expectScopedForwardReject(r);
}

// REJECT — the `set => call;` accessor form (an EXPRESSION body; the
// `set => do … end;` form runs the full pass and is rejected on the base).
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           expression_setter_forwards_call_bounded_index_param_to_scoped_rejected_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: int "
        "  get => 1; "
        "  set => (new Sink()).Take(q); "
        "end; ",
        kSetUse));
    expectScopedForwardReject(r);
}

// ---- RETURN: "returned" -------------------------------------------------------

// REJECT — the rev-m44-9083 RETURN shape: the indexer's own `=> q` body.
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           indexer_expression_body_returns_call_bounded_index_param_rejected_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: () -> int => q; ", kRetUse));
    expectCallBoundedReject(r, "returned");
}

// REJECT — the `get => q` accessor form.
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           expression_getter_returns_call_bounded_index_param_rejected_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: () -> int "
        "  get => q; "
        "end; ",
        kRetUse));
    expectCallBoundedReject(r, "returned");
}

// REJECT — the `get => do return q; end` accessor form.
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           block_getter_returns_call_bounded_index_param_rejected_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: () -> int "
        "  get => do return q; end; "
        "end; ",
        kRetUse));
    expectCallBoundedReject(r, "returned");
}

// ---- ADMIT controls: "may be invoked, bound to locals, and passed on as an
// argument to another call-bounded parameter" -----------------------------------

// ADMIT — a block getter INVOKES its call-bounded parameter and BINDS it to a
// local.  50 * (42 + 42) = 4200.
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           block_getter_invokes_and_binds_call_bounded_index_param_admitted_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: int "
        "  get => do var g := q; return q() + g(); end; "
        "end; ",
        kGetUse));
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4200\n"));
}

// ADMIT — a block getter PASSES its call-bounded parameter to another plain
// function-typed parameter and to ANOTHER INDEXER's index.  50 * 84 = 4200.
BEZEL_TEST(indexer_accessor_call_bounded_9084,
           block_getter_forwards_call_bounded_index_param_to_plain_param_and_index_admitted_spec_28_14) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: int "
        "  get => do var o := new Fl2(); return Sink.Plain(q) + o[q]; end; "
        "end; ",
        kGetUse));
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4200\n"));
}
