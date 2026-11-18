// fix #8767 (§28.3.10 *Owned local discharge requirement*, §28.9, v0.359): an
// owned local declared in a GETTER accessor body is never disposed — the
// getter body's scope is not drained.
//
// THE DEFECT. A property/indexer accessor body never reached the ownership
// dataflow: `checkMember`'s PropertyDecl arm ran only the NARROW structural
// checks on it (`checkGetterFreshOwnedReturn`, `scanGetterReturns`,
// `scanGetterSelfAssignments`, the closure-escape checks), never
// `MethodOwnership::run` — the method-body dataflow whose end-of-method
// scope-exit sweep sets `ld->autoDropAtScopeEnd`, the flag codegen's
// `lowerStmt` LocalDecl arm gates the plain-`var` scope-end disposal on.
// So an owned-state local in a getter body got NO end-of-scope disposal
// obligation: its heap block was never reclaimed.
//
// MEASURED on the base (r30-c1 @ 80439bb9d), the issue's own repro verbatim:
// "Direct leak of 16 byte(s) in 1 object(s) allocated from: #1 bz$prop$Item$get"
// — one 16-byte `Cell` per property read.
//
// THE SPEC. §28.3.10 (v0.359) *Owned local discharge requirement*: "When an
// owned local — or an `owned` parameter — reaches a *normal-exit* scope end
// without having reached a discharge point, and is not in moved-from state, it
// is disposed implicitly at that scope end"; and "On a path that never moves
// it out, a plain `var` owned local (or undischarged `owned` parameter) thus
// behaves like a `using var`: reclaimed when its scope ends." §28.9 (v0.359):
// "**Accessor faces are uniform across type kinds.** ... the two accessor
// body spellings of §23.3 (`get => e` and `get => do … end`) are
// semantically indistinguishable at every read site." — the block form's scope
// is a scope like any other.
//
// THE FIX. A getter block body that passes the CONFINEMENT PROOF
// (`getterBodyIsConfined`, src/ownership/Ownership_GetterConfinement.cpp)
// runs the method-body dataflow (`MethodOwnership::run`), which computes the
// ownership state, the discharge set and the auto-drop flag, and keeps the
// §28.3.10 must-use lint at the new implicit-drop site. The proof is an
// allow-list: no borrow-carrying value derived from a local of the body is
// handed to code outside the body, and every owned value is created by a
// constructor that does not publish `this`. A body outside the allow-list is
// left as it was before the fix (nothing armed: the bounded leak), because a
// getter body carries none of the §28.3 rejections that make the scope-end
// free sound in a method body (#8850).
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output and exit value as well as the leak count, so a
// silent zero-fill cannot pass, and the rows LOOP (200 reads of the property)
// so a one-shot leak cannot hide under the sanitizer's noise.
//
// THE NO-DOUBLE-DISPOSE ROWS. §28.3.10's rule is conditional: the scope-end
// disposal runs only for a local that reaches the scope end still OWNED and
// not in moved-from state. A local that was DISCHARGED — moved out
// (`var u := move t;`), explicitly disposed (`t?.Dispose();`) — must not be
// disposed a second time. The rows below pin both: each counts the
// destructor's "disposed" prints, so a double dispose shows up as a second
// print (and an ASan double-free report).

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

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired. `runAotChecked` reports an abort and any sanitizer report as
// `ran == false`.
void didNotCrash(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    didNotCrash(r, exitCode, out);
    assertNoLeak(r);
}

// How many times the property is read. A leak repro loops (lane contract:
// 50-500x) so a one-shot leak cannot hide under the sanitizer's noise.
constexpr int kReads = 200;

// The issue's own repro, VERBATIM, with the read LOOPED (the issue's repro
// reads the property once; the loop is what makes the leak measurable). The
// `Cell` class is the issue's own — no destructor, so `Cell` is a
// trivial-dispose class (§28.9) and the scope-end obligation is the
// compiler-inserted free alone.
std::string issueRepro() {
    return std::string(
        "module P; "
        "import System; "
        "class Cell "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end; "
        "end Cell; "
        "class H "
        "  public property Item: Cell? "
        "    get => do "
        "      var t: Cell? := new Cell(1); "
        " "
        "      return null; "
        "    end; "
        "  end Item; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var i := 0; "
        "    while i < ") + std::to_string(kReads) + " do " +
        "      Console.WriteLine(h.Item = null); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
}

// The same shape, with a DESTRUCTOR on `Cell`. `Cell` is then non-trivial-
// dispose, so the §28.3.10 must-use lint fires at the new implicit-drop site
// (the lint is a lint over a sound program — the drop still runs), and the
// destructor prints "disposed" once per read — the count pins that the
// disposal RAN, not merely that the leak disappeared.
std::string issueReproWithDestructor() {
    return std::string(
        "module P; "
        "import System; "
        "class Cell "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end Cell; "
        "class H "
        "  public property Item: Cell? "
        "    get => do "
        "      var t: Cell? := new Cell(1); "
        " "
        "      return null; "
        "    end; "
        "  end Item; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var i := 0; "
        "    while i < ") + std::to_string(kReads) + " do " +
        "      Console.WriteLine(h.Item = null); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
}

// The MOVED twin: `t` is transferred into `u` with the marker, so `t` is in
// moved-from state at the getter body's scope end and §28.3.10's scope-end
// disposal must SKIP it ("the move analysis leaves a transferred value in
// moved-from state so the scope-end disposal skips it; nothing is disposed
// twice"), while `u` — now the owner — is disposed exactly once.
std::string movedLocalRepro() {
    return std::string(
        "module P; "
        "import System; "
        "class Cell "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end Cell; "
        "class H "
        "  public property Item: Cell? "
        "    get => do "
        "      var t: Cell? := new Cell(1); "
        "      var u: Cell? := move t; "
        " "
        "      return null; "
        "    end; "
        "  end Item; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var i := 0; "
        "    while i < ") + std::to_string(kReads) + " do " +
        "      Console.WriteLine(h.Item = null); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
}

// The EXPLICITLY-DISPOSED twin: `t` is disposed by hand inside the getter
// body (§28.3's consume table, K3.9), so it is NOT undischarged at the scope
// end and the scope-end disposal must not run a second time. GREEN on the
// base as well (the explicit dispose already reclaims the block) — this is a
// BOUNDARY CONTROL, kept as a gtest row because a corpus case cannot express
// a dispose COUNT over 200 iterations; it goes RED under the plausible wrong
// fix "auto-drop every owned local in a getter body, ignoring the discharge
// set" (two "disposed" prints per read, and an ASan double-free report).
std::string explicitlyDisposedLocalRepro() {
    return std::string(
        "module P; "
        "import System; "
        "class Cell "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end Cell; "
        "class H "
        "  public property Item: Cell? "
        "    get => do "
        "      var t: Cell? := new Cell(1); "
        "      t?.Dispose(); "
        " "
        "      return null; "
        "    end; "
        "  end Item; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var i := 0; "
        "    while i < ") + std::to_string(kReads) + " do " +
        "      Console.WriteLine(h.Item = null); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
}

// The expected output of the trivial-dispose variant: one "True" per read
// (`h.Item = null` is the equality test — `=` is equality in Bezel).
std::string expectedTrueLines(int n) {
    std::string out;
    for (int i = 0; i < n; ++i)
        out += "True\n";
    return out;
}

// The expected output of the destructor variants: the getter's scope-end
// disposal runs BEFORE the read's result reaches the caller, so each read
// prints "disposed" and then "True".
std::string expectedDisposedThenTrue(int n) {
    std::string out;
    for (int i = 0; i < n; ++i)
        out += "disposed\nTrue\n";
    return out;
}

}  // namespace

// THE REGRESSION ROW: the issue's own repro, verbatim, looped. RED on the
// base (the Cell leaks on every read — 16 B each), GREEN with the fix.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_body_plain_var_owned_local_is_reclaimed_at_scope_end) {
    auto r = runAotChecked(issueRepro());
    expectClean(r, 0, expectedTrueLines(kReads));
}

// The same repro with a destructor on `Cell`: the disposal RAN (one "disposed"
// per read), not merely the leak gone. RED on the base (0 disposals + the
// leak), GREEN with the fix.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_body_owned_local_runs_its_destructor_at_scope_end) {
    auto r = runAotChecked(issueReproWithDestructor());
    expectClean(r, 0, expectedDisposedThenTrue(kReads));
}

// The MOVED twin: exactly ONE "disposed" per read — the moved-from `t` is not
// disposed again and the new owner `u` is disposed exactly once. RED on the
// base (0 disposals + the leak), GREEN with the fix.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_body_moved_local_is_not_disposed_twice) {
    auto r = runAotChecked(movedLocalRepro());
    expectClean(r, 0, expectedDisposedThenTrue(kReads));
}

// The EXPLICITLY-DISPOSED twin (a boundary control — green on both arms):
// exactly ONE "disposed" per read, so the scope-end disposal does not run on
// top of the explicit one.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_body_explicitly_disposed_local_is_not_disposed_twice) {
    auto r = runAotChecked(explicitlyDisposedLocalRepro());
    expectClean(r, 0, expectedDisposedThenTrue(kReads));
}

// ---------------------------------------------------------------------------
// fix-forward (§28.3, §28.3.10, v0.359): A GETTER LOCAL WHOSE BORROW CAN ESCAPE
// KEEPS NO SCOPE-END FREE.
//
// THE REJECTION. The first attempt (0ddb37e6d) ran the method-body dataflow
// over the getter body in a mode that ARMS the §28.3.10 scope-end free and
// SUPPRESSES every rejection the same pass raises. But §28.3's
// borrow-lifetime ("A borrow cannot outlive its source"), closure-sink,
// borrowed-field-store and consume-site-freeze rejections are what make that
// free SOUND in a method body. With them suppressed, a borrow of the freed
// local that leaves the getter dangles. r42-rev-m74 MEASURED seven such
// shapes; this lane found three more (x40, x41, x43). On the base each is a
// bounded leak, and at 0ddb37e6d a heap-use-after-free or SEGV.
//
// THE FIX (as it stands after fix-forward 3, below). A getter body the
// confinement proof does not accept arms no scope-end free at all, and a
// confined body the dataflow would have rejected (a suppressed §28.3 error)
// has every flag the run wrote restored. The local is then leaked, as on the
// base: the safe direction. No rejection is added, because a new getter
// rejection is a spec-shaped change (#8850).
//
// ORACLE. `runAotCheckedLenient`: generated-code AddressSanitizer with
// LeakSanitizer OFF. These rows pin the ABSENCE OF A USE-AFTER-FREE; the
// residual leak is the documented safe direction, not a claim these rows
// make. Each row asserts the program's exact output (the sum over 200 reads,
// which a read of freed memory corrupts) and exit code 0. The programs are the
// reviews' probes verbatim, minus the final `Console.WriteLine(Cell.n)`,
// which would pin the leak count.
//
// THE CONTROL. Of the ten no-UAF rows the first fix-forward (770099b71) added,
// nine were green on r30-c1 (a leak there, never a UAF) and moved to the
// conformance corpus's candidates (owner rule, RUN.md 2026-11-12: at most two
// base-green controls per suite). `b30` stays as this suite's second named
// control, beside the explicitly-disposed row above. The plausible wrong fix
// it catches is 0ddb37e6d itself: arm the getter's scope-end free with the
// method pass's §28.3 rejections suppressed and nothing recorded in their
// place (a heap-use-after-free there).
// ---------------------------------------------------------------------------

namespace {

void didNotUseAfterFree(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}
// b30_store_borrow_into_field.bzl
const char* const k_repro_b30 = R"BZL(
module P;
import System;
class Cell
  public static n: int := 0;
  public v: int;
  public constructor(x: int) this.v := x; Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
class Util
  public static method Sink(owned c: Cell) Console.Write(""); end Sink;
  public static method Peek(c: Cell): int => c.v;
end Util;
class H
  public b: Cell?;
  public property V: int
    get => do
      var t := new Cell(5);
      b := t;
      return 1;
    end;
  end V;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      acc := acc + h.V; if h.b <> null then acc := acc + h.b!.v; end;
    end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
}  // namespace

// THE SECOND NAMED CONTROL (green on r30-c1; RED at 0ddb37e6d, a
// heap-use-after-free). r42-rev-m74 probe b30 stores `b := t` into a plain
// field of `this`, read by the caller after the getter returns. §28.3 rejects
// the method twin ("borrow of `t` stored into a field would outlive its
// source").
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_borrow_stored_into_a_field_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_b30);
    didNotUseAfterFree(r, "1200\n");
}

// ---------------------------------------------------------------------------
// fix-forward 3 (§28.3, §28.3.10, v0.359): THE FREE IS ARMED ONLY IN A GETTER
// BODY PROVED CONFINED.
//
// THE REJECTION. r42-rev-s77 MEASURED eight new heap-use-after-frees at
// 5254d9c65. Fix-forwards 1 and 2 each added an ESCAPE SCREEN (a deny-list of
// the routes by which a borrow leaves the body), and each missed routes: the
// RECEIVER of a call or property read whose callee stores `this` (r01, r03,
// r04, r17), a user operator's operand (r09, r19), a lambda body's own local
// (r11, r20). The method twins of r01, r09 and r17 are ACCEPTED (§28.3
// field-store clause 3 is a trust admission), so no suppressed rejection
// marks those bodies either.
//
// THE FIX. The deny-list is inverted into an ALLOW-LIST that runs BEFORE the
// dataflow: a body is confined only when every expression that carries a
// borrow of one of its locals stays inside the body (a field read, a `null`
// test, an alias local, a non-escaping local lambda) and every owned value
// is a `new` of a class whose constructors and destructor do not publish
// `this`. Any other body keeps the pre-#8767 behaviour.
//
// THE ROWS. Each program has TWO getters on one class. `V` is the reviewer's
// probe body verbatim: its local must NOT be freed (the object is still
// reachable from `hd.c`, which `Peek()` reads after `V` returns). `W` holds a
// plain owned `Probe` local nothing else can reach: it MUST be reclaimed.
// The program prints the sum over 200 reads and then `Probe.n`, the count of
// live `Probe` objects.
//   * r30-c1 (no getter free at all): "1800\n200\n" — RED, `W` leaks.
//   * 5254d9c65 (the refuted screen): heap-use-after-free in `Peek()` — RED.
//   * this fix: "1800\n0\n", no memory error — GREEN.
// `runAotCheckedLenient`: generated-code AddressSanitizer with LeakSanitizer
// off; the rows do not pin `V`'s residual leak, which is the conservative
// answer and not a claim about the spec.
//
// The six no-UAF rows fix-forward 2 (5254d9c65) added — s02, s21, s23, s22,
// s05, s20 — were green on r30-c1 and moved to the conformance corpus's
// candidates (the two-control cap above).
// ---------------------------------------------------------------------------

namespace {

// `vBody` is the block body of the getter `V`.
std::string twoGetterProgram(const std::string& vBody) {
    return std::string(R"BZL(
module P;
import System;
class Holder
  public c: Cell?;
  public constructor() end constructor;
  public method Read(): int
    if c <> null then return c!.v; end;
    return 0;
  end Read;
  public static method operator +(h: Holder, x: Cell): int
    h.c := x;
    return 1;
  end operator +;
end Holder;
class Cell
  public v: int;
  public peer: Holder?;
  public constructor(x: int) this.v := x; end constructor;
  public method Remember() if peer <> null then peer!.c := this; end; end Remember;
  public property Self: int get => do if peer <> null then peer!.c := this; end; return 1; end; end Self;
end Cell;
class Probe
  public static n: int := 0;
  public v: int;
  public constructor(x: int) this.v := x; Probe.n := Probe.n + 1; end constructor;
  destructor Probe.n := Probe.n - 1; end destructor;
end Probe;
class H
  public owned hd: Holder := new Holder();
  public method Make(): owned Cell
    var c := new Cell(5);
    c.peer := hd;
    return c;
  end Make;
  public method Peek(): int return hd.Read(); end Peek;
  public property V: int
    get => do
)BZL") + vBody + R"BZL(
    end;
  end V;
  public property W: int
    get => do
      var p := new Probe(3);
      return p.v;
    end;
  end W;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      acc := acc + h.V + h.Peek() + h.W;
    end;
    Console.WriteLine(acc);
    Console.WriteLine(Probe.n);
    return 0;
  end Main;
end Program;
)BZL";
}

// 200 reads of (V = 1) + (Peek() = 5) + (W = 3), then no live `Probe`.
const char* const kTwoGetterOutput = "1800\n0\n";

}  // namespace

// r42-rev-s77 probe r01: `t.Remember()` — the RECEIVER of a method call whose
// callee stores `this` into a field (`peer!.c := this`). The method twin is
// accepted (§28.3 field-store clause 3). heap-use-after-free at 5254d9c65,
// whose screen looked at call arguments only.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_that_is_the_receiver_of_a_call_storing_this_is_not_freed_and_a_confined_sibling_getter_local_is_reclaimed) {
    auto r = bezel::test::runAotCheckedLenient(twoGetterProgram(
        "      var t := Make();\n"
        "      t.Remember();\n"
        "      return 1;\n"));
    didNotUseAfterFree(r, kTwoGetterOutput);
}

// r42-rev-s77 probe r09: `hd + t` — the operand of a USER operator whose body
// stores it (`h.c := x`). heap-use-after-free at 5254d9c65, whose screen did
// not treat a user operator as a call.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_that_is_a_user_operator_operand_the_operator_stores_is_not_freed_and_a_confined_sibling_getter_local_is_reclaimed) {
    auto r = bezel::test::runAotCheckedLenient(twoGetterProgram(
        "      var t := new Cell(5);\n"
        "      var r := hd + t;\n"
        "      return 1;\n"));
    didNotUseAfterFree(r, kTwoGetterOutput);
}

// r42-rev-s77 probe r11: the owned local lives INSIDE a block lambda the
// getter invokes, and the lambda body stores it into a field of `hd`. §28.3
// rejects the method twin; 5254d9c65 recorded the suppressed rejection but
// its withdrawal walk never entered lambda bodies: heap-use-after-free.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_lambda_body_local_stored_into_a_field_is_not_freed_and_a_confined_sibling_getter_local_is_reclaimed) {
    auto r = bezel::test::runAotCheckedLenient(twoGetterProgram(
        "      var g := lambda(): int => do\n"
        "        var u := new Cell(5);\n"
        "        hd.c := u;\n"
        "        return u.v;\n"
        "      end;\n"
        "      var r := g();\n"
        "      return 1;\n"));
    didNotUseAfterFree(r, kTwoGetterOutput);
}

// r42-rev-s77 probe r17: `t.Self` — the receiver of a PROPERTY read whose
// getter stores `this`. The method twin is accepted. heap-use-after-free at
// 5254d9c65.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_that_is_the_receiver_of_a_property_read_storing_this_is_not_freed_and_a_confined_sibling_getter_local_is_reclaimed) {
    auto r = bezel::test::runAotCheckedLenient(twoGetterProgram(
        "      var t := Make();\n"
        "      var r := t.Self;\n"
        "      return 1;\n"));
    didNotUseAfterFree(r, kTwoGetterOutput);
}
