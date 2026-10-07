// Codegen — spec §28.14 (v0.315) + §28.9 + §35.2, stdlib §2.20:
// the four compiler-provided `Result<T, E>` value-payload combinators
// (`Map`, `MapError`, `AndThen`, `UnwrapOr`) must not leak the closure
// cell they mint for a lambda-literal callback argument.
//
// THE DEFECT. `tryLowerResultCombinatorCall`
// (src/codegen/LlvmBackend_Calls.cpp) is dispatched at
// LlvmBackend_Calls2.cpp:7132 — BEFORE the method-symbol call path, because
// these four members have no `MethodSymbol`. It lowers the callback
// argument with `lowerExpr(*call.args[0].value)`, which for a lambda
// literal runs `lowerLambda` and `calloc`s the 48-byte closure cell
// `{fn, env, rc, envdtor, kind}` with `rc := 1` (the producer's reference,
// §28.9), and then never dropped it.
//
// The ordinary call path discharges that same reference: `lowerCall`
// collects a function-typed argument into `callBoundedClosureArgs` and
// emits `bz_closure_release` for each on the normal-return edge
// (LlvmBackend_Calls2.cpp ~14784, the #1073/#4078 release site), then
// `consumeLastLambdaCapturedCells()` for the capture-cell half (#3498).
// The combinator path returns before any of that, so every combinator
// call carrying a lambda literal leaked the cell.
//
// WHY A LEAK IS A DEFECT AND NOT AN OBLIGATION. Stdlib §2.20 is explicit
// that these members are call-bounded and that no disposal obligation
// arises: "The function parameters are plain, hence call-bounded (spec
// §28.14) — invoked during the call, never retained", and "every result is
// a fresh copyable value — no disposal obligation arises anywhere in a
// combinator call". §28.14's rule is one-directional — it constrains what
// the CALLEE may do with the callback — so nothing in it relieves the
// PRODUCER of its own +1. The producer's reference is dropped at the call
// site, exactly as on the ordinary path. Nothing here reclassifies
// `Result<int, string>`: §35.4 rule 3 keeps it value-like and
// copyable, and the fix changes no classification.
//
// WHAT THE LEAK WAS NOT. It is NOT the `Error("bad")` payload and NOT the
// `Map` result: the leak fires identically on the `Ok` arm, where the
// payload is a bare `int` and the pass-through never runs. The 48 bytes
// are the closure cell alone (located by disassembling the ASan build:
// `calloc(1, 0x30)` writing `lam$0` at +0 and a null env at +8, with no
// matching `free`).
//
// ORACLE. `runChecked` is `runAotChecked`, which sets `sanitizeAddress` on
// the GENERATED program and treats ANY sanitizer finding — including
// LSan's exit-time leak report — as `ran == false`. A leaking program
// therefore reports -999. This is a leak, so the leak oracle is the right
// one; `leakBytesBelow` (a floor on the reported byte count) is carried
// alongside on the primary row to pin the MAGNITUDE, and `r.ran` is
// asserted directly on the rows that take the checked path, so no row can
// pass by never running the sanitizer. Every leak row is looped 250x —
// a one-shot LSan row can pass on both arms of a fix.

#include <algorithm>
#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// LEAK oracle: ANY sanitizer finding (LSan included) ⇒ ran=false ⇒ -999.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// LEAK-MAGNITUDE oracle, floor direction. `leaksAtLeast` (the shape used by
// class_field_record_string_release_4253_test.cpp) asserts that a leak of at
// least N bytes IS present, so it is green on a broken tree and RED once the
// fix lands — it cannot be a regression row on its own. A regression row
// needs the opposite polarity, paired with `ran`:
//
//   * pre-fix  : the leak is present and large  → the row FAILS
//   * post-fix : no finding at all (-1)         → the row PASSES
//
// and asserting `ran` separately is what keeps it honest — without it, a
// harness that never ran the sanitizer would read -1 and pass vacuously.
// Never pin an exact byte count (LSan's conservative-scan jitter); assert a
// FLOOR comfortably below the observed minimum.
bool leakBytesBelow(const std::string& program, long floorBytes) {
    auto r = runAotChecked(program);
    if (r.ran) return true;  // no sanitizer finding at all — nothing to floor
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    long worst = -1;
    bool sawLeakSummary = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak)) {
            sawLeakSummary = true;
            worst = std::max(worst, std::stol(m[1].str()));
        }
    }
    // A finding that is NOT a leak summary (a UAF, an OOB) is a different
    // defect; report it as above the floor so the row fails rather than
    // passing on a crash it was not written to describe.
    if (!sawLeakSummary) return false;
    return worst < floorBytes;
}

}  // namespace

// ===========================================================================
// Leak rows: 250 iterations each. `acc` accumulates a value that DEPENDS on
// the combinator having actually run, so the body cannot be constant-folded
// away and the exit code is a behavioural oracle, not just a leak oracle.
// ===========================================================================

// The issue's own repro shape, amplified: `Result<int, string>` in the
// ERROR state, `.Map(lambda(x) => x * 2)`, never destructured. Pre-fix the
// Error arm still minted the closure cell and never dropped it — the leak
// is arm-independent — so this leaked 250 × 48 = 12000 bytes. `UnwrapOr`
// returns the fallback 3 on every Error, so acc = 750, and 750 mod 100 = 50.
BEZEL_TEST(result_combinator_callback_closure_release,
           map_error_arm_inline_lambda_no_leak_spec_28_14) {
    BEZEL_EXPECT_EQ(runChecked(R"(
module M;
class C public static method Main(): int
  var i := 1;
  var acc := 0;
  var r: Result<int, string> := Result<int, string>.Error("bad");
  while i <= 250 do
    var r2 := r.Map(lambda(x) => x * 2);
    acc += r2.UnwrapOr(3);
    i += 1;
  end;
  return acc mod 100;
end Main; end C;
)"), 50);
}

// The OK arm, where the callback is actually INVOKED. Doubles as the
// behavioural pin: acc = Σ 2i for i in 1..250 = 62750, so exit 50 is only
// reachable if `f` ran on every iteration. Pre-fix this leaked the same
// 250 × 48 bytes, which is what rules out "the leak is the Error payload
// being passed through".
BEZEL_TEST(result_combinator_callback_closure_release,
           map_ok_arm_inline_lambda_no_leak_spec_28_14) {
    BEZEL_EXPECT_EQ(runChecked(R"(
module M;
class C public static method Main(): int
  var i := 1;
  var acc := 0;
  while i <= 250 do
    var r: Result<int, string> := Result<int, string>.Ok(i);
    var r2 := r.Map(lambda(x) => x * 2);
    acc += r2.UnwrapOr(0);
    i += 1;
  end;
  return acc mod 100;
end Main; end C;
)"), 50);
}

// A CAPTURING callback. Two extra allocations ride along: the boxed
// capture cell for `k` and the env block pointing at it. Pre-fix these
// leaked on top of the closure cell (measured: 60 bytes in 3 allocations
// for a single `Map(lambda(x) => x * k)`). This row pins the
// `consumeLastLambdaCapturedCells()` half of the fix, which is a
// DIFFERENT mechanism from the closure-cell release.
//
// The receiver is `Ok(i)` and the callback is `x => x * k` with `k := i`,
// so each iteration yields i² and acc = Σ i² for i in 1..250 =
// 250·251·501/6 = 5239625, which mod 100 is 25. The non-zero, non-trivial
// remainder is the behavioural half: it is reachable only if the captured
// `k` was read correctly on every iteration.
BEZEL_TEST(result_combinator_callback_closure_release,
           map_capturing_lambda_no_leak_spec_28_14) {
    BEZEL_EXPECT_EQ(runChecked(R"(
module M;
class C public static method Main(): int
  var i := 1;
  var acc := 0;
  while i <= 250 do
    var k := i;
    var r: Result<int, string> := Result<int, string>.Ok(i);
    var r2 := r.Map(lambda(x) => x * k);
    acc += r2.UnwrapOr(0);
    i += 1;
  end;
  return acc mod 100;
end Main; end C;
)"), 25);
}

// AndThen: the same call-bounded callback, different member — the closure
// runs on the Ok arm and its RESULT is the combinator's result. acc =
// Σ 3i = 94125, so 94125 mod 100 = 25. Behavioural pin that the fix did
// not disturb the closure-call lowering.
BEZEL_TEST(result_combinator_callback_closure_release,
           andthen_inline_lambda_no_leak_spec_28_14) {
    BEZEL_EXPECT_EQ(runChecked(R"(
module M;
class C public static method Main(): int
  var i := 1;
  var acc := 0;
  while i <= 250 do
    var r: Result<int, string> := Result<int, string>.Ok(i);
    var r2 := r.AndThen(lambda(x) => Result<int, string>.Ok(x * 3));
    acc += r2.UnwrapOr(0);
    i += 1;
  end;
  return acc mod 100;
end Main; end C;
)"), 25);
}

// MapError: the callback is invoked on the ERROR arm, on a `string`
// payload, mapping it to `long`. Pins the third closure-carrying member,
// and that the string payload passing through the combinator is not
// itself what leaked. acc counts the 250 Error results = 250, so
// 250 mod 100 = 50.
BEZEL_TEST(result_combinator_callback_closure_release,
           maperror_inline_lambda_no_leak_spec_28_14) {
    BEZEL_EXPECT_EQ(runChecked(R"(
module M;
class C public static method Main(): int
  var i := 1;
  var acc := 0;
  while i <= 250 do
    var r: Result<int, string> := Result<int, string>.Error("bad");
    var r2 := r.MapError(lambda(e) => long(e.Length));
    acc += (if r2.IsError() then 1 else 0);
    i += 1;
  end;
  return acc mod 100;
end Main; end C;
)"), 50);
}

// MAGNITUDE row, paired with the first row above. The program is identical;
// only the oracle differs. `leakBytesBelow` reports the sanitizer's leak
// total, or -1 when there was no finding at all.
//
// The pre-fix tree reports 12000 bytes (250 × the 48-byte cell). The floor
// sits well below that so LSan's scan jitter cannot flip the row, and high
// enough that an unrelated small leak cannot satisfy it — so the assertion
// discriminates THIS defect specifically:
//
//   post-fix : -1  (clean)  → -1 < 10000, and `ran` is true  → row passes
//   pre-fix  : 12000        → 12000 >= 10000                  → row FAILS
//
// This is the row that makes the pair non-vacuous in the magnitude
// dimension: `runChecked` alone would also fail on any sanitizer finding,
// so it cannot distinguish "the 48-byte cell is gone" from "the sanitizer
// stopped working". `ran` is asserted alongside so the row can never pass
// by the sanitizer not running at all.
BEZEL_TEST(result_combinator_callback_closure_release,
           map_error_arm_leak_below_pre_fix_floor_spec_28_14) {
    const std::string kProg = R"(
module M;
class C public static method Main(): int
  var i := 1;
  var acc := 0;
  var r: Result<int, string> := Result<int, string>.Error("bad");
  while i <= 250 do
    var r2 := r.Map(lambda(x) => x * 2);
    acc += r2.UnwrapOr(3);
    i += 1;
  end;
  return acc mod 100;
end Main; end C;
)";
    auto r = runAotChecked(kProg);
    // The sanitizer leg RAN and reached the program — the row is not
    // vacuous, and the byte count below is a real measurement.
    BEZEL_EXPECT_TRUE(r.ran);
    // -1 (no finding) is below any floor; pre-fix this reads 12000.
    BEZEL_EXPECT_TRUE(leakBytesBelow(kProg, 10000));
}

// UnwrapOr control: its argument is an ordinary T VALUE, not a callback
// (stdlib §2.20 — "an ordinary value argument, not a deferred computation
// … the argument expression is evaluated at the call site"), so there is
// no closure cell to leak. This row is green BOTH pre- and post-fix and
// exists to pin that the fix did not over-reach: the release is gated on
// the callback-carrying members and on `isPlusOneClosureProducer`, so
// UnwrapOr's value argument is untouched. acc = 250 × 3 = 750 → 50.
BEZEL_TEST(result_combinator_callback_closure_release,
           unwrapor_value_fallback_untouched_spec_28_14) {
    BEZEL_EXPECT_EQ(runChecked(R"(
module M;
class C public static method Main(): int
  var i := 1;
  var acc := 0;
  var r: Result<int, string> := Result<int, string>.Error("bad");
  while i <= 250 do
    acc += r.UnwrapOr(3);
    i += 1;
  end;
  return acc mod 100;
end Main; end C;
)"), 50);
}

// BORROWED-callback control, and the sharpest boundary on the fix. Here
// the callback is a closure-typed LOCAL, so the combinator receives a
// BORROW of the local's cell — the local's own scope-end release owns the
// reference. The release added by the fix is gated on
// `isPlusOneClosureProducer`, which is false for an identifier, so no
// second release is emitted and there is no double-free. This is the row
// that would catch an ungated fix.
//
// ORACLE, and why this row is not a leak row. Known limitation, NOT
// introduced and NOT claimed fixed here: the local's own scope-end release
// is suppressed once the local is passed into ANY call, so this program
// leaks 250 × 48 bytes BOTH before and after the fix. That is a separate,
// combinator-independent defect — the same program passing that local to
// an ordinary user method with a function-typed parameter leaks
// identically, with no Result involved, which is how it was isolated from
// this one. So `leaksAtLeast`-style oracles are wrong here (they would
// pin a bug another lane owns), and `runChecked` is wrong too (it reads
// the known leak as a failure). What this row DOES pin is the property
// the fix must not break: no double-free. So it uses the CRASH-FREEDOM
// oracle — `runAotCheckedLenient`, which runs with detect_leaks=0 and
// therefore tolerates the known leak while still reporting a
// double-free / use-after-free / OOB. Both assertions are load-bearing:
// the exit code pins the behaviour (the combinator still ran 250 times and
// still returned the fallback 3), the lenient run pins the memory safety.
BEZEL_TEST(result_combinator_callback_closure_release,
           map_borrowed_local_callback_no_double_free_spec_28_14) {
    const std::string kProg = R"(
module M;
class C public static method Main(): int
  var f := lambda(x: int) => x * 2;
  var i := 1;
  var acc := 0;
  var r: Result<int, string> := Result<int, string>.Error("bad");
  while i <= 250 do
    var r2 := r.Map(f);
    acc += r2.UnwrapOr(3);
    i += 1;
  end;
  return acc mod 100;
end Main; end C;
)";
    // CRASH-FREEDOM only (detect_leaks=0): an ungated release would free the
    // local's cell at every iteration while the loop still holds it, so the
    // 2nd iteration would read freed memory — ASan reports it, `ran` is
    // false, and this row fails.
    BEZEL_EXPECT_TRUE(runAotCheckedLenient(kProg).ran);
    // Behaviour: acc = 250 × 3 = 750, so 750 mod 100 = 50.
    auto r = runAot(kProg);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 50);
}