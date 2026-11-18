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
// THE FIX. The getter body runs the method-body dataflow
// (`MethodOwnership::run`), which computes the ownership state, the discharge
// set and the auto-drop flag. The getter mode keeps the §28.3.10 must-use lint
// at the new implicit-drop site and suppresses the method-body-ONLY rules (the
// §926 comment records that a getter body runs "a NARROW getter-return check,
// not the full pass, so existing getters/setters surface no unrelated new
// diagnostics"; bringing them under the full pass is "a separate blast" per
// the §5252 comment).
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
// THE FIX. When the getter run would have rejected the body (a suppressed
// §28.3 error), or a borrow-carrying value leaves the body through a `return`
// or a store into a non-local location (`getterBorrowMayEscape`, which covers
// the method-pass holes #8836 / #8837), every scope-end free the run armed is
// withdrawn. The local is then leaked, as on the base: the safe direction.
// No rejection is added, because a new getter rejection is a spec-shaped
// change.
//
// ORACLE. `runAotCheckedLenient`: generated-code AddressSanitizer with
// LeakSanitizer OFF. These rows pin the ABSENCE OF A USE-AFTER-FREE; the
// residual leak is the documented safe direction, not a claim these rows
// make. Each row asserts the program's exact output (the sum over 200 reads,
// which a read of freed memory corrupts) and exit code 0. Each is RED at
// 0ddb37e6d (heap-use-after-free / SEGV) and GREEN with the fix-forward.
// The programs are r42-rev-m74's probes verbatim, minus the final
// `Console.WriteLine(Cell.n)`, which would pin the leak count.
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
// b04_return_borrow_of_locals_field.bzl
const char* const k_repro_b04 = R"BZL(
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
class Box
  public owned inner: Cell?;
  public constructor() this.inner := new Cell(7); end constructor;
end Box;
class H
  public property Inner: Cell?
    get => do
      var t := new Box();
      return t.inner;
    end;
  end Inner;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      var c := h.Inner; if c <> null then acc := acc + c.v; end;
    end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
// b34_return_method_borrow_of_local.bzl
const char* const k_repro_b34 = R"BZL(
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
class Box
  public owned inner: Cell?;
  public constructor() this.inner := new Cell(7); end constructor;
  public method Get(): Cell? => inner;
end Box;
class H
  public property Inner: Cell?
    get => do
      var t := new Box();
      return t.Get();
    end;
  end Inner;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      var c := h.Inner; if c <> null then acc := acc + c.v; end;
    end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
// b09_local_captured_by_returned_lambda.bzl
const char* const k_repro_b09 = R"BZL(
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
  public property F: () -> int
    get => do
      var t := new Cell(8);
      return lambda() => t.v;
    end;
  end F;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      var f := h.F; acc := acc + f();
    end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
// b33_return_lambda_capturing_alias.bzl
const char* const k_repro_b33 = R"BZL(
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
  public property F: () -> int
    get => do
      var t := new Cell(8);
      var b := t;
      return lambda() => b.v;
    end;
  end F;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      var f := h.F; acc := acc + f();
    end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
// b31_return_conditional_over_local.bzl
const char* const k_repro_b31 = R"BZL(
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
  public k: int;
  public property Item: Cell?
    get => do
      k := k + 1;
      var t := new Cell(6);
      return (if k > 0 then t else null);
    end;
  end Item;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      var c := h.Item; if c <> null then acc := acc + c.v; end;
    end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
// b28_borrow_escapes_nested_scope.bzl
const char* const k_repro_b28 = R"BZL(
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
  public property V: int
    get => do
      var b: Cell? := null;
      if true then
        var t := new Cell(3);
        b := t;
      end;
      return b!.v;
    end;
  end V;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      acc := acc + h.V;
    end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
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
// x41_alias_escapes_nested_scope.bzl
const char* const k_repro_x41 = R"BZL(
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
      var x: Cell? := null;
      if true then
        var t := new Cell(3);
        var a := t;
        x := a;
      end;
      return x!.v;
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
// x43_alias_stored_into_this_field.bzl
const char* const k_repro_x43 = R"BZL(
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
      var a := t;
      this.b := a;
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
// x40_lambda_body_stores_local_into_field.bzl
const char* const k_repro_x40 = R"BZL(
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
      var f := lambda() => do b := t; end;
      f();
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

// r42-rev-m74 probe b04 returns `t.inner`, a borrow of the local's OWNED
// field. §28.3 rejects it in a method body ("borrow returned from `t` would
// outlive its source").
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_returned_borrow_of_the_locals_owned_field_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_b04);
    didNotUseAfterFree(r, "1400\n");
}

// r42-rev-m74 probe b34 returns `t.Get()`, a method result that borrows the
// local's owned field. §28.3 rejects the method twin.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_returned_method_borrow_of_the_local_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_b34);
    didNotUseAfterFree(r, "1400\n");
}

// r42-rev-m74 probe b09 returns `lambda() => t.v`. §28.3's closure sink rule
// rejects the method twin ("returned closure captures `t` whose source is
// disposed at scope end").
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_returned_lambda_capturing_the_local_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_b09);
    didNotUseAfterFree(r, "1600\n");
}

// r42-rev-m74 probe b33 returns `lambda() => b.v` over an alias `b := t`.
// The method twin is ACCEPTED too (the pre-existing hole #8837), so no
// suppressed rejection marks it; the structural escape screen does.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_returned_lambda_capturing_an_alias_of_the_local_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_b33);
    didNotUseAfterFree(r, "1600\n");
}

// r42-rev-m74 probe b31 returns `(if k > 0 then t else null)`. The method
// twin is ACCEPTED too (the pre-existing hole #8836); the structural escape
// screen marks it.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_returned_if_expression_over_the_local_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_b31);
    didNotUseAfterFree(r, "1200\n");
}

// r42-rev-m74 probe b28 stores `b := t` from a nested scope into an outer
// local and reads `b!.v` after the scope ends. §28.3 rejects the method twin
// ("borrow held by `b` would outlive its source `t`").
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_borrow_escaping_a_nested_scope_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_b28);
    didNotUseAfterFree(r, "600\n");
}

// r42-rev-m74 probe b30 stores `b := t` into a plain field of `this`, read
// by the caller after the getter returns. §28.3 rejects the method twin
// ("borrow of `t` stored into a field would outlive its source").
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_borrow_stored_into_a_field_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_b30);
    didNotUseAfterFree(r, "1200\n");
}

// Lane probe x41 is b28 through an alias (`var a := t; x := a;`).
// Found by this lane: 0ddb37e6d is a heap-use-after-free, the base a leak.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_alias_escaping_a_nested_scope_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_x41);
    didNotUseAfterFree(r, "600\n");
}

// Lane probe x43 is b30 through an alias (`var a := t; this.b :=
// a;`). Found by this lane: 0ddb37e6d is a heap-use-after-free, the base a
// leak.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_alias_stored_into_a_field_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_x43);
    didNotUseAfterFree(r, "1200\n");
}

// Lane probe x40 stores the local into a field of `this` from inside
// a block lambda invoked in the getter (`lambda() => do b := t; end`). Found
// by this lane: 0ddb37e6d is a heap-use-after-free, the base a leak.
BEZEL_TEST(getter_body_owned_local_auto_drop_8767,
           getter_local_lambda_body_storing_the_local_into_a_field_is_not_freed_under_it) {
    auto r = bezel::test::runAotCheckedLenient(k_repro_x40);
    didNotUseAfterFree(r, "1200\n");
}
