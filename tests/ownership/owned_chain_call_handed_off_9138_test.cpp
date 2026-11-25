// Regression test for issue #9138 — an OWNED chain-call result that is
// RETURNED, DISCARDED (`_ :=`), or ASSIGNED into an owned field or an
// existing owned local was disposed TWICE: once by the CG60 chain-call arm's
// own scope-end disposal (on its temp slot `ccres.slot`) and once by the
// destination's own rule (the return slot / the discard / the owning
// location's owner / the local's affine auto-drop).
//
// THE SHAPE.
//
//   module M;
//   public class W public V: int := 3; end W;
//   public class C
//     public static method Make(): () -> owned W
//       return lambda(): W => new W();
//     end Make;
//     public static method Get(): owned W
//       return C.Make()();          // the RETURN position
//     end Get;
//   end C;
//
// `Make(): () -> owned W` is a FUNCTION VALUE whose declared return slot
// carries the `owned` marker, so §28.3.5's announcement table (row 1) makes
// the invocation's result OWNED — the caller is to dispose it.
//
// THE DEFECT. The CG60 chain-call arm in `lowerCallDispatch`
// (src/codegen/LlvmBackend_Calls2.cpp) stores the result in its own
// `ccres.slot` alloca and registers a SCOPE-END disposal on it — that
// registration is §28.3.11's unbound-owned-temporary disposal. It fired for
// EVERY owned chain-call result that was not the initializer of a local, so
// in every position where the value is HANDED OFF to a destination that owns
// it, the value was disposed twice.
//
// THE SPEC (§28.3.11, v0.359, *Unbound owned temporaries*):
//
//   "An owned value may also be produced as an **unbound temporary**: the
//    result of an `owned`-returning call (or other owned producer) that is not
//    bound to any name, not `move`d, and not returned (a value transferred
//    into a `return` is held by the return slot, below), but used only by
//    *borrowing* operations within the expression …"
//
//   "A temporary that is instead bound (to a `var`/`using var`), `move`d,
//    returned, or transferred into an owning location is governed by the
//    corresponding transfer or discharge rule and is not disposed under this
//    rule (it would otherwise be disposed twice)."
//
// ⇒ In the RETURN, DISCARD and ASSIGNMENT positions the value is disposed
// EXACTLY ONCE, by the destination's own rule:
//   * §28.3.11 *The return slot*: "A value transferred into a `return` — a
//     fresh producer … — leaves the named-value regime above for a callee-local
//     owned slot, established on the cleanup stack at the `return` statement."
//   * §17.13 *The discard `_`*: "the discard form `_ := expr;` disposes the
//     value at the end of the discard statement for any class or interface
//     type".
//   * §28.8 *Ownership transfer*: a store into an OWNED location transfers the
//     value; the location's owner disposes it.
//
// The plain-call twin shows the intended shape: for a plain `owned`-returning
// call (`Make(): owned W`), `lowerCallDispatch` NEVER registers a scope-end
// disposal for the result — the destination owns it alone. Only the chain-call
// arm added the extra registration.
//
// MEASURED on the base (r30-c1 @ 55fa97066), `bezelc --sanitize=address`,
// each program looped 200×:
//
//   p04_return   `return C.Make()();`      → ERROR: AddressSanitizer:
//                                             heap-use-after-free (the callee
//                                             frees the value it returns)
//   p06_discard  `_ := C.Make()();`         → ERROR: AddressSanitizer:
//                                             heap-use-after-free
//   p03_field    `h.F := C.Make()();`       → ERROR: AddressSanitizer:
//                                             heap-use-after-free
//   t09_reassign `w := C.Make()();`         → ERROR: AddressSanitizer:
//                                             heap-use-after-free
//
// THE ORACLE. `runAotChecked` — the checked AOT path with the
// AddressSanitizer over the GENERATED module and LeakSanitizer at exit. The
// rows LOOP (200 iterations inside the program), so neither the
// use-after-free nor a leak can hide under the sanitizer's noise, and the
// leak leg is what proves the value is still disposed EXACTLY ONCE after the
// fix (a fix that simply stopped disposing the value would pass a
// use-after-free check and fail this one).
//
// The plain `runAot` oracle is NOT used for these rows: the double disposal is
// a double FREE, which the allocator reports as an abort — but the abort is
// the allocator's, not the compiler's, and its text is not the defect. The
// checked oracle names the defect exactly ("heap-use-after-free").
//
// The row for the REASSIGN position uses `var w := new W();` for the local's
// FIRST value, not the issue's own `var w := C.Make()();`. The two differ in
// one respect that is NOT this issue's: §28.7's drop-on-overwrite
// (`disposeDisplacedOwnedClassLocal`) classifies an owned local by its
// INITIALIZER FORM, and a CHAIN-CALL initializer is not one of the forms it
// recognises, so the value DISPLACED by the reassignment is never disposed —
// a pre-existing LEAK, present on the base for the same program written
// `var w := C.Make()(); w := new W();` (no double free there at all), filed
// separately. This row's spelling keeps the displaced value inside the forms
// the drop-on-overwrite covers, so the row measures the DISPOSAL COUNT of the
// reassigned chain-call result — the thing #9138 is about — and nothing else.

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

// The class the chain call mints: a field read AFTER the chain call, so the
// rows observe the object the double free corrupted.
const char* kClass =
    "public class W "
    "  public V: int := 3; "
    "end W; ";

// The chain-call producer the issue's own program uses.
const char* kProducer =
    "public static method Make(): () -> owned W "
    "  return lambda(): W => new W(); "
    "end Make; ";

// ROW 1's program — the issue's LITERAL repro: `return C.Make()();` out of a
// method whose return slot is `owned W`. The callee hands the value to its
// caller, so the CALLER's local disposes it (the plain-call twin
// `return Make();` over an `owned`-returning method does exactly this).
std::string returnedProgram() {
    return std::string("module M; ") + kClass +
        "public class C "
        "  " + kProducer +
        "  public static method Get(): owned W "
        "    return C.Make()(); "
        "  end Get; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var w := C.Get(); "
        "      t := t + w.V; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// ROW 2's program — `_ := C.Make()();`: the discard disposes the value at the
// end of the discard statement (§17.13), so the chain-call arm must not.
std::string discardedProgram() {
    return std::string("module M; ") + kClass +
        "public class C "
        "  " + kProducer +
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      _ := C.Make()(); "
        "      t := t + 1; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// ROW 3's program — `h.F := C.Make()();` into an `owned` field: the store
// transfers the value into the field, and the field's owner (the `h` the
// loop body creates) disposes it.
std::string ownedFieldProgram() {
    return std::string("module M; ") + kClass +
        "public class C "
        "  " + kProducer +
        "  public owned F: W? := null; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var h := new C(); "
        "      h.F := C.Make()(); "
        "      t := t + 1; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// ROW 4's program — `w := C.Make()();` into an EXISTING owned local: the
// store transfers the value into the local, whose affine auto-drop disposes
// it at the scope end (§28.3.10). The local's first value is a plain `new`
// so that §28.7's drop-on-overwrite covers the DISPLACED value too (see the
// file header).
std::string reassignedProgram() {
    return std::string("module M; ") + kClass +
        "public class C "
        "  " + kProducer +
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var w := new W(); "
        "      t := t + w.V; "
        "      w := C.Make()(); "
        "      t := t + w.V; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// CONTROL 1 — the BOUND-LOCAL twin: `var w := C.Make()();`. The local binds
// the result, so the local's own regime disposes it (#9082). This row is
// GREEN on the base and GREEN after the fix; it is red under a wrong fix
// that registers the chain-call arm's scope-end disposal IN ADDITION to the
// local's own (the double free of #9082).
std::string boundLocalProgram() {
    return std::string("module M; ") + kClass +
        "public class C "
        "  " + kProducer +
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var w := C.Make()(); "
        "      t := t + w.V; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// CONTROL 2 — the UNBOUND twin: the chain-call result is used by a BORROWING
// operation inside the expression (a member read), so it stays an unbound
// owned temporary and §28.3.11 disposes it at the end of the full
// expression. This row is GREEN on the base and GREEN after the fix; it is
// red under a wrong fix that drops the chain-call arm's registration for
// EVERY owned chain-call result (200 minted frames would leak).
std::string unboundProgram() {
    return std::string("module M; ") + kClass +
        "public class C "
        "  " + kProducer +
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      t := t + C.Make()().V; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ROW 1 — the RETURN position (the issue's literal repro), looped 200 times.
//
// RED on the base: the ASan heap-use-after-free of the issue — the callee
// disposes the value it transfers into its return slot, and the caller then
// reads the freed object.
BEZEL_TEST(owned_chain_call_handed_off_9138,
           returned_chain_call_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(returnedProgram());
    expectClean(r, 0, "600\n");
}

// ROW 2 — the DISCARD position, looped 200 times.
//
// RED on the base: the ASan heap-use-after-free — the discard disposes the
// value at the end of the discard statement and the chain-call arm's
// scope-end disposal reads the freed object.
BEZEL_TEST(owned_chain_call_handed_off_9138,
           discarded_chain_call_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(discardedProgram());
    expectClean(r, 0, "200\n");
}

// ROW 3 — the OWNED-FIELD position, looped 200 times.
//
// RED on the base: the ASan heap-use-after-free — the field's owner disposes
// the transferred value and the chain-call arm's scope-end disposal reads the
// freed object.
BEZEL_TEST(owned_chain_call_handed_off_9138,
           owned_field_chain_call_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(ownedFieldProgram());
    expectClean(r, 0, "200\n");
}

// ROW 4 — the EXISTING-OWNED-LOCAL position, looped 200 times.
//
// RED on the base: the ASan heap-use-after-free — the local's affine
// auto-drop disposes the transferred value and the chain-call arm's
// scope-end disposal reads the freed object.
BEZEL_TEST(owned_chain_call_handed_off_9138,
           reassigned_owned_local_chain_call_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(reassignedProgram());
    expectClean(r, 0, "1200\n");
}

// CONTROL 1 — the BOUND-LOCAL twin stays exactly-once (see the helper's
// comment).
BEZEL_TEST(owned_chain_call_handed_off_9138,
           bound_local_twin_is_still_disposed_exactly_once) {
    JitResult r = runAotChecked(boundLocalProgram());
    expectClean(r, 0, "600\n");
}

// CONTROL 2 — the UNBOUND twin stays exactly-once (see the helper's
// comment).
BEZEL_TEST(owned_chain_call_handed_off_9138,
           unbound_temporary_twin_is_still_disposed_exactly_once) {
    JitResult r = runAotChecked(unboundProgram());
    expectClean(r, 0, "600\n");
}

// ---------------------------------------------------------------------------
// The NOT-handed-off positions (fix-forward of the first #9138 cut).
//
// A hand-off is right only where a destination really takes the value. The
// first cut also handed the result off in four positions where NOTHING takes
// it, so once the CG60 arm's registration was suppressed nobody disposed the
// value and every iteration leaked it (MEASURED under LSan on that cut: 200
// leaked allocations, destructor count 0; the base before it: clean, 200):
//
//   * a UNIT expression body — `lambda() => C.Make()();` at a `() -> ()` slot,
//     a unit arrow method `method Fire() => C.Make()();`, and an async lambda
//     at a `() -> Task` slot. The body's value is not returned (a unit callable
//     has no return value; §30.6's no-value `Task` completes with no result),
//     so it is §28.3.11's unbound temporary: "the result of an
//     `owned`-returning call … that is not bound to any name, not `move`d, and
//     not returned … Such a temporary is disposed at the end of the full
//     expression in which it is produced".
//   * a `:=` into a PLAIN parameter `p := C.Make()();`. A plain parameter is a
//     borrow of the caller's value (§28.3); it disposes nothing, so it is not
//     an owning location and the store is not a transfer into one.
//
// `W` counts its destructor runs, so each row checks "exactly once" both ways:
// no leak (LSan) and the count printed is the number of values produced.

namespace {

const char* kCountingClass =
    "public class W "
    "  public static Drops: int := 0; "
    "  public V: int := 3; "
    "  destructor "
    "    W.Drops := W.Drops + 1; "
    "  end destructor; "
    "end W; ";

std::string unitLambdaBodyProgram() {
    return std::string("module M; ") + kCountingClass +
        "public class C "
        "  " + kProducer +
        "  public static method Main(): int "
        "    var f: () -> () := lambda() => C.Make()(); "
        "    for var j in 0..<200 do "
        "      f(); "
        "    end; "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

std::string unitArrowMethodProgram() {
    return std::string("module M; ") + kCountingClass +
        "public class C "
        "  " + kProducer +
        "  public static method Fire() => C.Make()(); "
        "  public method Shoot() => C.Make()(); "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    for var j in 0..<200 do "
        "      C.Fire(); "
        "      c.Shoot(); "
        "    end; "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

std::string asyncUnitLambdaBodyProgram() {
    return std::string("module M; ") + kCountingClass +
        "public class C "
        "  " + kProducer +
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task := async lambda() => C.Make()(); "
        "    for var j in 0..<200 do "
        "      await f(); "
        "    end; "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

std::string plainParameterAssignProgram() {
    return std::string("module M; ") + kCountingClass +
        "public class C "
        "  " + kProducer +
        "  public static method Touch(p: W): int "
        "    p := C.Make()(); "
        "    return p.V; "
        "  end Touch; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var k := new W(); "
        "    for var j in 0..<200 do "
        "      t := t + C.Touch(k); "
        "    end; "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// RED on the first cut (200 leaked allocations, count 0): the lambda's unit
// body handed its value off as "returned", and the lambda then dropped it.
BEZEL_TEST(owned_chain_call_not_handed_off_9138,
           unit_lambda_expression_body_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(unitLambdaBodyProgram());
    expectClean(r, 0, "200\n");
}

// RED on the first cut (400 leaked allocations, count 0): the static and the
// instance unit arrow method each handed the value off as "returned".
BEZEL_TEST(owned_chain_call_not_handed_off_9138,
           unit_arrow_method_body_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(unitArrowMethodProgram());
    expectClean(r, 0, "400\n");
}

// RED on the first cut (200 leaked allocations, count 0): a no-value `Task`
// completes with no result, so the async body's value is not the task's.
BEZEL_TEST(owned_chain_call_not_handed_off_9138,
           async_unit_lambda_expression_body_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(asyncUnitLambdaBodyProgram());
    expectClean(r, 0, "200\n");
}

// RED on the first cut (200 leaked allocations, count 0): the `:=` arm
// handed the value off to a plain parameter, which disposes nothing.
BEZEL_TEST(owned_chain_call_not_handed_off_9138,
           plain_parameter_assign_result_is_disposed_exactly_once) {
    JitResult r = runAotChecked(plainParameterAssignProgram());
    expectClean(r, 0, "600\n200\n");
}
