// §28.3 K7.4 / K7.5 (v0.359): a move inside an operand that is NOT ALWAYS
// EVALUATED — the right operand of `??` (§7.7), of `and` / `or` (§17.2), the
// argument list of a `?(` / `?.` call (§7.5), an unthrown `throw e` arm —
// leaves the binding MAYBE-CONSUMED, never moved-from: "owned ⊓ moved-from
// ... = maybe-consumed — unreadable after the join ...; scope-end disposal
// per-path". The binding must therefore still be disposed on the path that
// did not evaluate the operand (issues #8986, #8993).
//
// THE BUG. `recordMovesIn` (the move classifier of the LocalDecl-initializer
// and assignment-RHS positions) walked every such operand as if it always
// ran, so the binding was MOVED-FROM after the construct on both paths and no
// scope-end disposal was emitted for it: on the path that skipped the operand
// the value leaked. The fix meets the operand's walked exit with the state in
// which it never ran (the same `joinMarkerFreeArmEnds` the marker-free
// if-expression arms use) and flags the local for K7.4's per-path disposal.
//
// OBSERVABILITY. Each program runs both paths 200 times (a one-shot leak is a
// vacuous oracle) and prints the number of live `Res` instances after the
// loop (a destructor counter): WITHOUT the fix it is 200 (one leaked `Res` per
// skipping iteration) and runAotChecked's LSan flips `r.ran` to false; WITH
// the fix it is 0 and the run is sanitizer-clean. The `throw e` rows leak an
// exception, which no counter sees, so they rest on the LSan leg alone.
//
// The `return`-operand spellings of the same programs were leak-free before
// the fix (the discharge walk recorded no operand moves at all) and are
// spec-decided controls: they live in the conformance corpus, not here.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAotChecked — AOT + ASan/LSan
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

const char* kPrelude =
    "module M; "
    "import System; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; "
    "  end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "  public static method Pass(owned r: Res): owned Res return r; end Pass; "
    "  public static method MakeOrNull(c: bool): owned Res? "
    "    if c then return new Res(1); end; "
    "    return null; "
    "  end MakeOrNull; "
    "end Sink; ";

// `F(c)` runs `body` with `k` non-null and `h` / `cb` set exactly when `c`.
// The lambda is block-bodied (an expression-bodied lambda fails IR
// verification on the LLVM 23 base, #8995) and adopts its `owned` parameter
// with `using var`, because a lambda's undischarged `owned` parameter is not
// auto-dropped today (#8865 / #8800) — so the only leak a row can observe is
// the one under test.
std::string program(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public cb: ((owned Res) -> int)? := null; "
           "  public method TakeM(owned r: Res): int return r.tag; end TakeM; "
           "  public method F(c: bool): int "
           "    var k: int? := null; if c then k := 5; end; "
           "    var h: C? := null; if c then h := this; end; "
           "    this.cb := null; "
           "    if c then "
           "      this.cb := lambda(owned r: Res): int => do "
           "        using var q := move r; return q.tag; "
           "      end; "
           "    end; " +
           body +
           "  end F; "
           "  public static method Main(): int "
           "    using var o := new C(); "
           "    var s := 0; "
           "    for var i in 0..<200 do s += o.F(true); s += o.F(false); end; "
           "    Console.WriteLine(s); "
           "    Console.WriteLine(Res.live); "
           "    return 0; "
           "  end Main; "
           "end C; ";
}

std::string lastLine(const std::string& s) {
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    const auto p = t.rfind('\n');
    return p == std::string::npos ? t : t.substr(p + 1);
}

// Sanitizer-clean, the expected sum, and no live `Res` after the loop.
void expectDisposedOnEveryPath(const JitResult& r, const std::string& sum) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(sum + "\n") != std::string::npos);
    BEZEL_EXPECT_EQ(lastLine(r.stdoutText), "0");
}

bool hasMaybeConsumedReadError(const OwnershipResult& r,
                               const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("local `" + name +
                           "` may have already been consumed on another "
                           "branch and may not be read here") !=
                std::string::npos)
            return true;
    return false;
}

}  // namespace

// #8986, the unselected `??` arm moves `y`: `MakeOrNull(c)` is non-null when
// c = true, so only the left arm runs (§7.7) and `y` must be disposed at
// scope end; when c = false the right arm transfers `y` into the result.
// (The issue's literal Repro A spells the arms `move x ?? move y`; that
// marker is rejected in `??`-operand position by axiom A7 (v0.359) — not
// enforced today, tracked separately — so the row uses the admitted spelling
// of the same unselected-arm move: a call argument inside the arm.)
BEZEL_TEST(conditional_operand_move_8986,
           unselected_coalesce_arm_move_is_disposed_spec_28_3_k7_4) {
    JitResult r = runAotChecked(program(
        "    var y := new Res(2); "
        "    var t := Sink.Take(Sink.MakeOrNull(c) ?? Sink.Pass(move y)); "
        "    return t;"));
    expectDisposedOnEveryPath(r, "600");
}

// #8986 Repro B: the unselected `??` arm is `throw e` (K7.5: a bottom-typed
// arm absorbs). When c = true it never runs and the exception that was never
// thrown must still be disposed; when c = false it is thrown and caught.
BEZEL_TEST(conditional_operand_move_8986,
           unthrown_coalesce_throw_arm_exception_is_disposed_spec_28_3_k7_5) {
    JitResult r = runAotChecked(program(
        "    try "
        "      var e := new InvalidOperationException(\"b\"); "
        "      var t := Sink.Take(Sink.MakeOrNull(c) ?? throw e); "
        "      return t; "
        "    catch "
        "      InvalidOperationException => do return 7; end "
        "    end try;"));
    expectDisposedOnEveryPath(r, "1600");
}

// #8993 b01: the `??` RIGHT operand is a call taking `move y`; it runs only
// when `k` is null (c = false).
BEZEL_TEST(conditional_operand_move_8993,
           coalesce_right_call_move_is_disposed_when_skipped_spec_7_7) {
    JitResult r = runAotChecked(program(
        "    var y := new Res(2); "
        "    var t := (k ?? Sink.Take(move y)) + 0; "
        "    return t;"));
    expectDisposedOnEveryPath(r, "1400");
}

// The assignment-RHS twin of b01.
BEZEL_TEST(conditional_operand_move_8993,
           coalesce_right_call_move_in_assignment_is_disposed_when_skipped) {
    JitResult r = runAotChecked(program(
        "    var y := new Res(2); "
        "    var t := 0; "
        "    t := k ?? Sink.Take(move y); "
        "    return t;"));
    expectDisposedOnEveryPath(r, "1400");
}

// #8993 b02: the `and` right operand runs only when `c` is true (§17.2).
BEZEL_TEST(conditional_operand_move_8993,
           and_right_call_move_is_disposed_when_short_circuited_spec_17_2) {
    JitResult r = runAotChecked(program(
        "    var x := new Res(1); "
        "    var b := c and Sink.Take(move x) > 0; "
        "    if b then return 1; end; "
        "    return 0;"));
    expectDisposedOnEveryPath(r, "200");
}

// #8993 b02, `or` form: the right operand runs only when `c` is false.
BEZEL_TEST(conditional_operand_move_8993,
           or_right_call_move_is_disposed_when_short_circuited_spec_17_2) {
    JitResult r = runAotChecked(program(
        "    var x := new Res(1); "
        "    var b := c or Sink.Take(move x) > 0; "
        "    if b then return 1; end; "
        "    return 0;"));
    expectDisposedOnEveryPath(r, "400");
}

// #8993 b03: `this.cb?(move x)` over a null `cb` evaluates no argument
// (§7.5); `cb` is null exactly when c = false.
BEZEL_TEST(conditional_operand_move_8993,
           null_conditional_call_argument_is_disposed_when_callee_null_7_5) {
    JitResult r = runAotChecked(program(
        "    var x := new Res(1); "
        "    var t := this.cb?(move x) ?? 0; "
        "    return t;"));
    expectDisposedOnEveryPath(r, "200");
}

// The `?.` member-call twin of b03: `h?.TakeM(move y)` over a null `h`.
BEZEL_TEST(conditional_operand_move_8993,
           null_conditional_member_call_argument_is_disposed_when_null_7_5) {
    JitResult r = runAotChecked(program(
        "    var y := new Res(2); "
        "    var t := (h?.TakeM(move y) ?? 0) + 0; "
        "    return t;"));
    expectDisposedOnEveryPath(r, "400");
}

// #8502: the same `?.` call at STATEMENT position (the discharge walk, not
// the initializer walk): `h?.TakeM(move y);` over a null `h` evaluates no
// argument, so `y` must be disposed at scope end on that path.
BEZEL_TEST(conditional_operand_move_8993,
           null_conditional_member_call_statement_argument_is_disposed_8502) {
    JitResult r = runAotChecked(program(
        "    var y := new Res(2); "
        "    h?.TakeM(move y); "
        "    return 1;"));
    expectDisposedOnEveryPath(r, "400");
}

// #8993 b04: `k ?? throw e` with `k` non-null (c = true) never throws; the
// thrown path (c = false) is caught. `e` is disposed on the unthrown path.
BEZEL_TEST(conditional_operand_move_8993,
           unthrown_throw_operand_exception_is_disposed_spec_28_3_k7_5) {
    JitResult r = runAotChecked(program(
        "    try "
        "      var e := new InvalidOperationException(\"b\"); "
        "      var t := k ?? throw e; "
        "      return t; "
        "    catch "
        "      InvalidOperationException => do return 7; end "
        "    end try;"));
    expectDisposedOnEveryPath(r, "2400");
}

// K7.4: after the meet the binding is maybe-consumed, so a read of it is
// rejected with the maybe-consumed diagnostic (before the fix it was
// reported as unconditionally "already moved out").
BEZEL_TEST(conditional_operand_move_8993,
           read_after_conditional_operand_move_is_maybe_consumed_k7_4) {
    OwnershipResult r = parseAndCheckOwnership(program(
        "    var y := new Res(2); "
        "    var t := k ?? Sink.Take(move y); "
        "    return t + y.tag;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMaybeConsumedReadError(r, "y"));
}

// #8601 (§28.3.10 / §28.8.1, v0.359): a `move y` argument inside a binary
// operand of a `return` transfers the obligation exactly as the binding
// spelling does. Before the fix the return walk did not record that move, so
// with an early return that moves `y` on the other path the scope end was
// rejected ("would be dropped here without disposal") — Repro B.
BEZEL_TEST(return_operand_move_credit_8601,
           binary_return_operand_move_with_early_return_compiles_and_runs) {
    JitResult r = runAotChecked(program(
        "    var y := new Res(2); "
        "    if c then return Sink.Take(move y); end; "
        "    return Sink.Take(move y) + 1;"));
    expectDisposedOnEveryPath(r, "1000");
}

// #8601 Repro A: the same move credited at the return, so no must-use
// warning attaches to `y` (§28.3.10 suppression 3: "it is moved or
// returned").
BEZEL_TEST(return_operand_move_credit_8601,
           binary_return_operand_move_draws_no_must_use_warning) {
    OwnershipResult r = parseAndCheckOwnership(program(
        "    var y := new Res(2); "
        "    return Sink.Take(move y) + 1;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    bool warned = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("owned value in `y` is implicitly disposed") !=
                std::string::npos)
            warned = true;
    BEZEL_EXPECT_FALSE(warned);
}
