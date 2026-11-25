// Regression test for issue #9177 — an OWNED chain-call result
// (`Make(): () -> owned W`, §28.3.5's announcement table row 1) as the
// RIGHT-HAND SIDE of a `??=` assignment (`w ??= C.Make()();`) emitted MALFORMED
// IR: the chain-call arm's `ccres.slot` alloca was created inside the `??=`
// arm's null branch (`coaleq.do`) while the scope-end disposal it registers is
// emitted at the SCOPE end — outside that branch, which the alloca does not
// dominate.
//
//   Instruction does not dominate all uses!
//     %ccres.slot = alloca ptr, align 8
//     %43 = load ptr, ptr %ccres.slot, align 8
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// MEASURED on the base (r30-c1 @ c0ef7b6df) with the issue's own repro: a
// compile-time ICE, on every `??=` shape that carries an owned chain-call
// result (a null local, a non-null local, an `owned` field, an auto-property,
// inside a loop).
//
// THE SPEC.
//
// §6.6 (v0.359): "`??=` is not a read-modify-write: `x ??= e` assigns `e` only
// when `x`'s current value is null, with the typing and ownership rules of
// §7.7"; §7.7 (v0.359): "The compound form `??=` evaluates its right-hand
// side, and invokes the setter, only when the assignment occurs"; and §28.3's
// producers row: "The compound form `a ??= b` assigns into `a` and
// follows the same meet rule for the assigned-into location". So the value the
// RHS produces is TRANSFERRED into the target: the target's owner disposes it.
//
// §28.3.11 *Unbound owned temporaries* (v0.359), on a value that IS handed off:
//
//   "A temporary that is instead bound (to a `var`/`using var`), `move`d,
//    returned, or transferred into an owning location is governed by the
//    corresponding transfer or discharge rule and is not disposed under this
//    rule (it would otherwise be disposed twice)."
//
// The chain-call arm (CG60 in `lowerCallDispatch`,
// src/codegen/LlvmBackend_Calls2.cpp) registers a §28.3.11 scope-end disposal
// for an owned chain-call result UNLESS the destination hands it off
// (`chainCallResultHandedOff_`, the #9138 mechanism). The `??=` arm did not
// hand it off, so in the `??=` position the value was disposed TWICE — once by
// the target's own rule and once by the chain-call arm's registration — and,
// before the fix, the ICE hid the second disposal.
//
// THE FIX. The `??=` arms (the generic one in `lowerStmt` and the
// accessor-mediated one, `lowerAccessorNullCoalescingAssign`) now hand the
// RHS's owned chain-call result off to the target, exactly as the `:=` arms
// do for the same target shapes. The value is then produced ONCE and disposed
// ONCE, by the target's owner.
//
// THE ORACLE. `runAotChecked` — the checked AOT path with the
// AddressSanitizer over the GENERATED module and LeakSanitizer at exit. Each
// program COUNTS its destructor runs, so "exactly once" is checked BOTH ways:
// the leak leg (LSan) catches a value nobody disposed, and the count catches a
// value disposed twice (the count would exceed the number of values
// produced). The rows LOOP, so neither can hide under the sanitizer's noise.
//
// The plain `runAot` oracle is not used here: a double disposal is a double
// FREE, which the allocator reports as an abort — but the abort is the
// allocator's, not the compiler's, and its text is not the defect. The
// checked oracle names the defect exactly ("heap-use-after-free").

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired. `runAotChecked`
// reports an abort and any sanitizer report as `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// The class the chain call mints. It COUNTS its destructor runs, so a row can
// check "disposed exactly once" and not merely "not leaked": a value disposed
// twice runs the destructor twice.
const char* kCountingClass =
    "public class W "
    "  public static Drops: int := 0; "
    "  public V: int := 3; "
    "  destructor "
    "    W.Drops := W.Drops + 1; "
    "  end destructor; "
    "end W; ";

// The chain-call producer the issue's own program uses: a FUNCTION VALUE whose
// declared return slot carries the `owned` marker, so §28.3.5's announcement
// table (row 1) makes the invocation's result OWNED.
const char* kProducer =
    "public static method Make(): () -> owned W "
    "  return lambda(): W => new W(); "
    "end Make; ";

// The rows' programs put the `??=` in a helper METHOD, so the target's scope
// — and with it the target's own scope-end disposal — has ENDED by the time
// `Main` reads the destructor count. A `??=` in `Main` itself would dispose
// the value after the last print, and the count would read 0.

// ROW 1's program — the ISSUE'S LITERAL REPRO: `w ??= C.Make()();` into a
// null local, in a 200-iteration loop. The `??=` assigns only on the FIRST
// iteration (the target is null then and non-null afterwards), so exactly ONE
// value is produced and it is disposed exactly once.
std::string nullLocalProgram() {
    return std::string("module M; ") + kCountingClass +
        "public class C "
        "  " + kProducer +
        "  public static method Run(): int "
        "    var t := 0; "
        "    var w: W? := null; "
        "    for var j in 0..<200 do "
        "      w ??= C.Make()(); "
        "      t := t + 1; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := C.Run(); "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// ROW 2's program — the same `??=` into a FRESH null local on every
// iteration: 200 values are produced, and each is disposed exactly once (the
// local's scope ends with the iteration). This is the row that cannot hide a
// leak under the sanitizer's noise: 200 allocations, 200 destructor runs, 0
// leaked bytes.
std::string freshLocalProgram() {
    return std::string("module M; ") + kCountingClass +
        "public class C "
        "  " + kProducer +
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var w: W? := null; "
        "      w ??= C.Make()(); "
        "      t := t + 1; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := C.Run(); "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// ROW 3's program — the `??=` into a NON-NULL target: §6.6/§7.7's short-circuit
// means the RIGHT-HAND SIDE IS NEVER EVALUATED, so no value is produced at
// all. The count is 1 — the target's OWN value, disposed by the target's own
// rule — and NOT 2, which is what a right-hand side that ran would leave.
// (Had the right-hand side run and its value been disposed twice, the count
// would be 3.)
std::string nonNullLocalProgram() {
    return std::string("module M; ") + kCountingClass +
        "public class C "
        "  " + kProducer +
        "  public static method Run(): int "
        "    var t := 0; "
        "    var w: W? := new W(); "
        "    for var j in 0..<200 do "
        "      w ??= C.Make()(); "
        "      t := t + 1; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := C.Run(); "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// ROW 4's program — the FIELD target: `c.F ??= C.Make()();` into an `owned`
// field. The store transfers the value into the field, and the field's owner
// (the object `c`) disposes it exactly once — §28.8 *Ownership transfer*.
// The field is declared `owned` because §28.5 makes a non-`owned` field a
// borrow, and a borrow cannot own the transferred value.
std::string ownedFieldProgram() {
    return std::string("module M; ") + kCountingClass +
        "public class C "
        "  " + kProducer +
        "  public owned F: W? := null; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    var c := new C(); "
        "    for var j in 0..<200 do "
        "      c.F ??= C.Make()(); "
        "      t := t + 1; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := C.Run(); "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// ROW 5's program — the ACCESSOR-MEDIATED target: `c.P ??= C.Make()();` into
// an auto-property. This shape does not go through the generic `??=` arm (a
// property has no backing address in the generic path) but through
// `lowerAccessorNullCoalescingAssign`, which lowers the right-hand side
// inside its own null branch (`coaleq.acc.do`) — the same shape of defect,
// and the same fix. The property is declared `owned` for the same reason the
// field above is: §28.5 makes a non-`owned` property a borrow, and a borrow
// cannot own the transferred value.
std::string propertyProgram() {
    return std::string("module M; ") + kCountingClass +
        "public class C "
        "  " + kProducer +
        "  public owned P: W? := null; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    var c := new C(); "
        "    for var j in 0..<200 do "
        "      c.P ??= C.Make()(); "
        "      t := t + 1; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := C.Run(); "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ROW 1 — the issue's literal repro, looped 200 times.
//
// RED on the base: the ICE — the module does not compile at all
// ("codegen error: IR verification failed"), so the program never runs.
// RED under a wrong fix that hoists the `ccres.slot` alloca to the entry
// block WITHOUT handing the value off: the value would then be disposed twice
// (the target's own rule and the chain-call arm's registration) and the
// destructor count would read 2 instead of 1.
BEZEL_TEST(owned_chain_call_coalesce_assign_9177,
           null_local_chain_call_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(nullLocalProgram());
    expectClean(r, 0, "200\n1\n");
}

// ROW 2 — 200 values produced, each disposed exactly once.
//
// RED on the base: the ICE.
// RED under a wrong fix that drops the chain-call arm's registration without
// handing the value off: 200 values would leak (LSan) and the destructor
// count would read 0.
BEZEL_TEST(owned_chain_call_coalesce_assign_9177,
           fresh_local_chain_call_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(freshLocalProgram());
    expectClean(r, 0, "200\n200\n");
}

// ROW 3 — the non-null target: the right-hand side is never evaluated.
//
// RED on the base: the ICE.
// RED under a wrong fix that evaluates the right-hand side eagerly (§6.6
// forbids it): the count would read 2 instead of 1.
BEZEL_TEST(owned_chain_call_coalesce_assign_9177,
           non_null_local_short_circuits_the_chain_call) {
    JitResult r = runAotChecked(nonNullLocalProgram());
    expectClean(r, 0, "200\n1\n");
}

// ROW 4 — the `owned` field target.
//
// RED on the base: the ICE.
// RED under a wrong fix that hoists the alloca without handing the value off:
// the value would be disposed twice (the field's owner and the chain-call
// arm's registration).
BEZEL_TEST(owned_chain_call_coalesce_assign_9177,
           owned_field_chain_call_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(ownedFieldProgram());
    expectClean(r, 0, "200\n1\n");
}

// ROW 5 — the accessor-mediated (auto-property) target.
//
// RED on the base: the ICE.
BEZEL_TEST(owned_chain_call_coalesce_assign_9177,
           property_target_chain_call_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(propertyProgram());
    expectClean(r, 0, "200\n1\n");
}
