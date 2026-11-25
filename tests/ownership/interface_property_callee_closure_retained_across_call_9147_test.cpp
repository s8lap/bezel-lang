// Regression test for issue #9147 — a closure INVOKED THROUGH A FUNCTION-TYPED
// PROPERTY read via an INTERFACE-typed receiver (`ih.P(ih)`) whose callee
// reassigns the property reads its freed environment (heap-use-after-free).
//
// THE DEFECT. A function value read out of a function-typed holder is a BORROW
// (§28.3, *Function-typed holders*: "A function value **read** from any holder —
// a field, a property or indexer … — carries the empty set"): the read takes no
// ownership, the holder keeps the reference. §28.9 (v0.359) says the same of
// an accessor, for EVERY declared type: "A property or indexer **read** is a
// lend for every declared type … a getter never transfers a reclamation
// obligation to its reader".
//
// The receiver's STATIC type decides which getter runs. With a CLASS-typed
// receiver the read is the class's own auto-property: the compiler-generated
// backing field holds an INDEPENDENT +1 on the closure cell (retained at store
// by `lowerClosureFieldStore`, released on the owner's disposal) and the read
// LENDS it — so #9124 taught the CALL to take a reference of its own for the
// duration of the call.
//
// With an INTERFACE-typed receiver the read dispatches, through the itable, to
// the OVERRIDE's getter. When that override is an auto-property, the read
// LENDS the override's backing field's reference — the same lend, one level
// up — but nothing took a reference for the call: the interface-dispatched
// read was not the class-typed read's twin in the compiler, and the call
// borrowed the only reference there was. So when the running closure
// reassigned the property (`x.Drop()` sets `P := lambda(x: IHx) => 0;`), that
// reference was released; if it was the last one, the closure cell was freed
// (with the capture cells §27.5 heap-allocates in it) WHILE THE BODY WAS STILL
// RUNNING, and the body's read of its captured variable was a use-after-free.
//
//   MEASURED on the base (r30-c1 @ e97a8efbc), `bezelc --sanitize=address` on
//   the issue's literal repro:
//
//     ERROR: AddressSanitizer: heap-use-after-free on address 0x7c0b553e0030
//     READ of size 4 at 0x7c0b553e0030 thread T0
//         #0 in lam$1
//         #1 in main
//     freed by thread T0 here:
//         #1 in bz$lam$1$envdtor
//     SUMMARY: AddressSanitizer: heap-use-after-free bezel in lam$1
//
// THE FIX. The call through an interface-dispatched property read takes a
// reference of its OWN before the invocation and gives it back after it
// (§28.3.11's discharge at the end of the full expression — here the end of
// the call expression), exactly as the class-typed auto-property twin does
// (#9124). The retain/release pair is BALANCED, so it is right whether the
// override's getter MINTS a fresh value or LENDS a holder's — the override is
// chosen at runtime, and the reader does not have to know which.
//
// THE SPEC.
//   §27.5 (v0.359): "Captured variables are heap-allocated in a closure
//   object" — the captured variable's storage IS the closure object, so the
//   body can only read it while that object exists.
//   §28.9 (v0.359): "Runtime management governs the *function value itself* —
//   the heap-allocated closure object that backs it" — so the object must
//   outlive every call that is still running through it: a function value must
//   not be freed while a call through it is running. And, for the read's face:
//   "A property or indexer **read** is a lend for every declared type … a
//   getter never transfers a reclamation obligation to its reader" — the
//   interface's declared type is the receiver's static type, so the
//   interface-dispatched read is a lend like the class-typed one.
//   §28.3 (v0.359), the *Function-typed holders* bullet: "A function value
//   **read** from any holder — a field, a property or indexer … — carries the
//   empty set" — the read is a borrow, so the holder may release its
//   reference while the call runs, and the CALL is then the only thing that
//   can keep the closure alive.
//   §28.3.11 (v0.359): the discharge of a reference taken for a full
//   expression falls at "the end of the full expression in which it is
//   produced" — here the end of the call expression.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that neither sanitizer fired, so a silent
// zero-fill cannot pass. The rows LOOP (200 iterations inside the program), so
// the defect cannot hide under the sanitizer's noise.
//
// `runAot` CANNOT be the oracle here: it does not run the AddressSanitizer
// pass over the generated module (only `runAotChecked` sets
// `opts.sanitizeAddress`), so the use-after-free would go unreported and the
// row would be green on the base — pinning nothing. This is the same reason
// the #9096 and #9124 rows (tests/ownership/
// field_callee_closure_retained_across_call_9096_test.cpp,
// auto_property_callee_closure_retained_across_call_9124_test.cpp) use the
// checked oracle, and the same rule the lane contract states for
// disposal/ownership work.

#include <iostream>
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

}  // namespace

// ROW 1 — the issue's literal repro. The receiver is an INTERFACE-typed local
// (`var ih: IHx := h;`), so the property read dispatches through the itable to
// the class's auto-property getter, and the closure is called through that
// read (`ih.P(ih)`). The body reassigns the property (`x.Drop()` sets
// `P := lambda(x: IHx) => 0;`) while it runs.
//
// The sum is `sum(i mod 3 + 1)` over 200 iterations = 399, and every
// iteration's `k + 1` is read out of the closure's environment — the read
// that the base turns into a heap-use-after-free.
//
// RED on the base: a heap-use-after-free in `lam$1` (freed by
// `bz$lam$1$envdtor`).
BEZEL_TEST(interface_property_callee_closure_retained_across_call_9147,
           interface_property_callee_displacing_the_property_is_a_uaf) {
    const std::string src =
        "module M; "
        "public interface IHx "
        "  property P: (IHx) -> int; "
        "  method Drop(); "
        "end IHx; "
        "public class Hx: IHx "
        "  public property P: (IHx) -> int := lambda(x: IHx) => 0; "
        "  public constructor() end constructor; "
        "  public method Drop() P := lambda(x: IHx) => 0; end Drop; "
        "  public method Fill(k: int) P := lambda(x: IHx) => do x.Drop(); return k + 1; end; end Fill; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); h.Fill(i mod 3); "
        "      var ih: IHx := h; "
        "      t := t + ih.P(ih); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "399\n");
}

// ROW 2 — the same interface-dispatched read, but the OVERRIDE is a COMPUTED
// property whose getter reads a FIELD (`property P: (IHx) -> int => _q;`)
// instead of an auto-property. The override kind is the other half of the
// matrix: a computed getter's read is a lend of the field's closure as well
// (§28.9: "a getter never transfers a reclamation obligation to its reader"),
// so the call needs a reference of its own here too — and the override is
// chosen at RUNTIME, so the call site cannot tell the two override kinds
// apart. This row pins that the fix is not specific to the auto-property
// override.
//
// RED on the base: a heap-use-after-free in `lam$1` (freed by
// `bz$lam$1$envdtor`).
BEZEL_TEST(interface_property_callee_closure_retained_across_call_9147,
           interface_property_over_a_computed_getter_displacing_the_field_is_a_uaf) {
    const std::string src =
        "module M; "
        "public interface IHx "
        "  readonly property P: (IHx) -> int; "
        "  method Drop(); "
        "end IHx; "
        "public class Hx: IHx "
        "  private _q: (IHx) -> int := lambda(x: IHx) => 0; "
        "  public property P: (IHx) -> int => _q; "
        "  public constructor() end constructor; "
        "  public method Drop() _q := lambda(x: IHx) => 0; end Drop; "
        "  public method Fill(k: int) _q := lambda(x: IHx) => do x.Drop(); return k + 1; end; end Fill; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); h.Fill(i mod 3); "
        "      var ih: IHx := h; "
        "      t := t + ih.P(ih); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "399\n");
}

// ROW 3 — the NULL-CONDITIONAL spelling of the same interface-dispatched read
// (`ih?.P(h)`, over a nullable interface-typed receiver and a nullable
// function-typed property). §7.5's `?.` spelling resolves the member as
// `ih.P` does, so the read dispatches through the itable exactly as the `.`
// spelling's does — and the nullable function type has no owner for a closure
// reference (#8763's fix-forward 2), so the getter mints nothing and the
// reader is left with no reference of its own.
//
// This is the SAME defect as row 1 in the OTHER spelling, and the fix reaches
// it because the `?.` call lowers the dotted member through the same call
// dispatch. (The CLASS-typed `!`-spelled nullable-property twin of this shape
// is #9109 — open, and not this lane's.)
//
// RED on the base: a heap-use-after-free in `lam$1` (freed by
// `bz$lam$1$envdtor`).
BEZEL_TEST(interface_property_callee_closure_retained_across_call_9147,
           null_conditional_interface_property_callee_displacing_the_property_is_a_uaf) {
    const std::string src =
        "module M; "
        "public interface IHx "
        "  property P: ((IHx) -> int)?; "
        "  method Drop(); "
        "end IHx; "
        "public class Hx: IHx "
        "  public property P: ((IHx) -> int)? := lambda(x: IHx) => 0; "
        "  public constructor() end constructor; "
        "  public method Drop() P := lambda(x: IHx) => 0; end Drop; "
        "  public method Fill(k: int) P := lambda(x: IHx) => do x.Drop(); return k + 1; end; end Fill; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); h.Fill(i mod 3); "
        "      var ih: IHx? := h; "
        "      t := t + (ih?.P(h) ?? 0); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "399\n");
}

// ROW 4 — the NO-DISPLACEMENT control: the same interface-dispatched read and
// the same call, with a body that does NOT displace the property while it
// runs. This is the shape the fix must leave alone, and it is RED under the
// plausible wrong fix that takes the reference and never gives it back (the
// "leak instead of use-after-free" direction): 200 unreleased references
// would leave 200 cells live at exit and LeakSanitizer would count them.
//
// GREEN on the base and with the fix; it pins that the ordinary
// interface-property call still runs, still returns the closure's own value,
// and still leaks nothing.
BEZEL_TEST(interface_property_callee_closure_retained_across_call_9147,
           interface_property_callee_without_displacement_is_unaffected) {
    const std::string src =
        "module M; "
        "public interface IHx "
        "  property P: (IHx) -> int; "
        "  method Drop(); "
        "end IHx; "
        "public class Hx: IHx "
        "  public property P: (IHx) -> int := lambda(x: IHx) => 0; "
        "  public constructor() end constructor; "
        "  public method Drop() P := lambda(x: IHx) => 0; end Drop; "
        "  public method Fill(k: int) P := lambda(x: IHx) => do return k + 1; end; end Fill; "
        "end Hx; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var h := new Hx(); h.Fill(i mod 3); "
        "      var ih: IHx := h; "
        "      t := t + ih.P(ih); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "399\n");
}
