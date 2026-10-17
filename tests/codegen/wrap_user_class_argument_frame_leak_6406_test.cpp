// Codegen — #6406 / §29.2 + §28.3 + §28.9 + §28.2 (v0.338).
//
// THE DEFECT, and the reason this file exists at all. #6406 reported that a
// USER CLASS implementing `IEnumerable<T>`, passed to an `IEnumerable<T>`
// FORMAL, leaked its §29.2 wrapper frame — 32 B per call. The report named the
// link `wrapMade == false ⇒ no registration ⇒ leak`, but explicitly recorded
// that the link was CODE READING, not measurement, and named the decisive
// experiment: one line, `*outWrapMade = true` before the user-class `return`.
//
// MEASURED HERE, and the measurement has a result the issue did not predict:
//
//   * The leak is REAL and reproduces byte-for-byte on the pre-fix tree.
//     Removing the one line and rebuilding gives, on the program below,
//       Direct leak of 9600 byte(s) in 300 object(s) allocated from:
//           #0 calloc  #1 main
//       SUMMARY: AddressSanitizer: 9600 byte(s) leaked in 300 allocation(s).
//     9600 / 300 = 32 = exactly one wrapper frame per call. (#6406's own probe
//     reported 9,632 B / 301 allocs; the same signature, one allocation of
//     harness difference.)
//
//   * The line ALREADY EXISTS on this branch's base. `git log -S` puts
//     `if (outWrapMade && wrapped && wrapped != v) *outWrapMade = true;`
//     at `src/codegen/LlvmBackend_Assign.cpp:9904` in commit 04d402b3
//     ("fix #6397"), which is this branch's own base — #6397 fixed the
//     PUBLISHER-COVERAGE gap for the for-in consumer face and, in the same
//     commit, gave this arm the `outWrapMade` report it had never had. The
//     issue was filed against `c7e4581b`, before that commit landed.
//
// So this is a COVERAGE commit, not a fix commit: the fix is upstream in
// 04d402b3, and what was missing was any row that would notice it being
// reverted. See "WHY THIS FILE IS NOT REDUNDANT" below — this exact shape
// was already in the suite and was blind to the leak.
//
// WHY THIS FILE IS NOT REDUNDANT. `iter_native_552, user_class_as_ienumerable_param`
// (tests/codegen/iterator_native_test.cpp) already compiles a user class
// passed to an `IEnumerable<int>` formal. It could not have caught this, for
// two reasons that are each independently sufficient:
//   (1) its oracle is `compileAndRun` — a bare `driver.compileFile` + system()
//       with NO ASan instrumentation and NO leak detection, so a 32-byte
//       per-call leak is invisible to it by construction; and
//   (2) it runs the call ONCE, and 32 B is far below the threshold that makes
//       a leak worth reporting even under LSan.
// Neither is a defect in that row, which pins what it was written to pin
// (§29.2 user-class vtable dispatch — the sum). It is simply not a leak
// oracle, and this file is.
//
// ORACLE. `runAotChecked` (ALWAYS ASan on the generated code) with
// `BEZEL_AOT_DETECT_LEAKS=strict` armed per row via `LeakEnv`, because
// `runAotChecked` passes `detect_leaks=0` BY DEFAULT (aot_support.hpp's
// `LeakDetectMode`) — a plain row would exit 0 on the very tree it exists to
// catch, which is worse than no row. `checkedToolchainSkipped` is asserted
// FALSE and INVERTED, BEFORE anything that could absorb the skip, so a box
// that cannot link the ASan binary fails loudly here instead of reporting a
// green row that certified nothing. No `LSAN_OPTIONS` is set anywhere: this
// leak is found at LSan defaults.
//
// NO ROW HERE ASSERTS THAT A LEAK IS PRESENT. Every row asserts the program
// RUNS CLEAN and prints the right sum. A row asserting the leak would invert
// the moment the defect is fixed — which, on this branch, it already has.
//
// §-strings live in COMMENTS only; a `§` inside an assertion would make the
// row a tripwire on a diagnostic rename.
#include <cstdlib>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row. Required, not tidiness:
// the gate may run a whole shard with the knob set, and a row that asserts
// knob-ON behaviour must not be polluted by (or pollute) its neighbours.
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

// A user class that DECLARES the enumerator protocol itself, so
// `wrapUserClassAsIEnumerable`'s structural scan resolves `enumTs == srcTs`
// (the `structurallyIsEnumerator(srcTs)` fast path, #2384) and the wrapper's
// slot-1 disposer resolves. This is the shape #6406's probe used, and it is
// also `wrap_property_read_is_a_lend_6387_test.cpp`'s `kSelfA`, reused
// verbatim on purpose: it makes this file's rows directly comparable with
// #6382's and #6397's, and it is NOT the #6382 mirror (`GetEnumerator`
// returns `this` and the shape that mattered there was a clone disposing a
// source it did not own).
const char* kSelfA =
    "public class SelfA: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfA; ";

// The callee. Its parameter is a pure `IEnumerable<int>` FORMAL, which is what
// routes the argument through `wrapAsIEnumerableForTarget` at the ARGUMENT
// face — `src/codegen/LlvmBackend_Assign.cpp`, the `wrapMade` registration
// immediately after `wrapAsIEnumerableForTarget(...)` in `buildOrderedArgs`
// (site 10 of the 13 `ds.iterEnumDispose = true` registrations; the other 12
// are 6 in `LlvmBackend_Stmts.cpp`, 3 in `LlvmBackend_Calls2.cpp`, 3 in
// `LlvmBackend_Setup.cpp`).
const char* kCallee =
    "public class Consume "
    "  public static method Total(e: IEnumerable<int>): int "
    "    var s := 0; "
    "    for var x in e do s := s + x; end; "
    "    return s; "
    "  end Total; "
    "end Consume; ";

// 300 iterations × (1 + 2 + 3) = 1800. The loop is not decoration: a single
// call leaks 32 B, and the row's whole claim is that 300 of them do not
// accumulate. `SelfA(3)` yields 1, 2, 3 on each pass because each iteration
// constructs a FRESH instance (a fresh `cur := 0`), so the sum is a real
// assertion on a run that genuinely ran — a body that never executed could not
// print 1800.
const char* kLoopMain =
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<300 do "
    "      var u := new SelfA(3); "
    "      s := s + Consume.Total(u); "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; ";

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

}  // namespace

// ===========================================================================
// THE PINNING ROW — a user class at an `IEnumerable<T>` formal.
//
// This is #6406's exact shape and its exact number. 32 B per call, 300 calls,
// 9,600 B — the wrapper frame the argument face minted and, with
// `wrapMade == false`, nobody reclaimed.
//
// §29.2 puts the frame's reclamation on the scope that mints it: the caller's
// scope-end `iterEnumDispose` (`using var __source` semantics), because
// §28.3 makes an `IEnumerable<T>` parameter a BORROW — "a non-`owned`
// argument position is a borrowing use whatever the callee's body does with
// the parameter" — so the callee never disposes it and the caller is the only
// candidate. §28.2 keeps every OTHER candidate out: `SelfA`'s own scope-end
// disposal is a holder for `SelfA`, not for the wrapper minted over it, and
// §28.9's "a getter never transfers a reclamation obligation to its reader"
// (here: an argument) says the same from the other side.
//
// The row asserts exit 0, no sanitizer finding, and `s=1800`. It does NOT
// assert a leak is present: on this branch's base the fix is already in
// (04d402b3), so such an assertion would be red today and would invert the
// moment the fix were reverted — which is exactly the direction a pin must
// not fail.
// ===========================================================================
BEZEL_TEST(wrap_user_class_argument_frame_leak_6406,
           user_class_at_ienumerable_formal_frees_its_wrapper_frame_spec_29_2) {
    LeakEnv scope("strict");
    JitResult r = checked("module TP; " + std::string(kSelfA) +
                          kCallee +
                          "public class TPImpl " + kLoopMain + "end TPImpl;");
    // Loud BEFORE any absorb: a silently-skipped leak oracle would certify
    // nothing while reporting green.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 300 × (1 + 2 + 3) — the loop genuinely ran.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1800") != std::string::npos);
}

// ===========================================================================
// CONTROL — the SAME callee, the SAME formal, the SAME loop, with an
// `Array<int>` receiver instead of a user class.
//
// `wrapAsIEnumerableForTarget`'s Array arm has always reported its mint
// (that is #2298, and it is what `wrap_argument_frame_leak_2298_test.cpp`
// pins), so this row is GREEN on both sides of the reverted line by
// construction. It is here for one reason: a change that reached past the
// user-class arm — into the shared registration, the `disposeStack_.back()`
// scope choice, or the slot-1 disposer itself — would move THIS row, and
// nothing else in this file would notice. It also keeps the file honest about
// what it is measuring: the difference between the pinning row and this one is
// the RECEIVER KIND and nothing else.
// ===========================================================================
BEZEL_TEST(wrap_user_class_argument_frame_leak_6406,
           array_at_ienumerable_formal_control_stays_clean_spec_29_2) {
    LeakEnv scope("strict");
    JitResult r = checked(
        "module TA; " + std::string(kCallee) +
        "public class TAImpl "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<300 do "
        "      var a: Array<int> := [1, 2, 3]; "
        "      s := s + Consume.Total(a); "
        "    end for; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end TAImpl;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1800") != std::string::npos);
}