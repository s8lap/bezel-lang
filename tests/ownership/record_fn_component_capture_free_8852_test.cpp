// Spec-derived regression test for issue #8852 — a record whose FUNCTION-TYPED
// component captures a scope-bound owned local escapes that local's scope
// unrejected.
//
// §9.7 (v0.359) states the rule for the component in its own words:
//
//   "A component whose declared type is a function type admits only a
//    capture-free function value, at construction and in `with`."
//
// §28.3.8 (v0.359) states the same rule as the sink rule's holder list —
// "a **record component**" is one of the function-typed holders — and names
// the two stores: "an argument for a record component at every call of the
// record's generated primary constructor (§9.1) and a `with` assignment to it
// (§9.5)".
//
// The hole: the record construction DROPPED the component closure's source
// set. §28.3.8's *Provenance* clause 5 reads a function value back from a
// component with the EMPTY set, so a capture-bearing closure stored in a
// component loses the bound that kept its capture alive — and the closure
// then outlives the local it captured.
//
// MEASURED on r30-c1 @ cba27ac1f (the issue's own repro): the program below
// compiled CLEAN (two warnings, no error) and its AOT+ASan run died with
// "ERROR: AddressSanitizer: SEGV on unknown address 0x00000000000c …
// #0 in lam$0" — the captured owned local `t` was disposed at the method's
// scope end and the escaped closure read it afterwards. The DIRECT form
// (`return new R(lambda() => t.v);`) was already rejected, by the §28.3
// escape check that descends into the construction's arguments.
//
// The fix rejects the store itself, so the local-bound and the field-store
// shapes are rejected where the closure is stored rather than only where it
// escapes.
//
// THE STORE SITE (fix-forward). §28.3.8 names ONE site for the construction
// store: "an argument for a record component at **every call of the record's
// generated primary constructor** (§9.1)". A record body may declare a
// constructor with a DIFFERENT parameter list beside the generated primary
// (§9.3: "A body constructor with a different parameter list is not required
// to chain to the primary"), and a call that resolved to such a DECLARED
// constructor binds its arguments to that constructor's PARAMETERS — §28.3.8:
// "a record's declared constructor is checked like any other body, its plain
// function-typed parameter being call-bounded" — and NOT to the components.
// The `declared_constructor_call_admits_capturing_argument_9_7` row below
// pins that: the argument is admitted there, and the store (if the declared
// constructor's body makes one) is judged in the body.
//
// The base-green controls (a capture-free closure, and a closure over a
// value-typed local, still admitted into a function-typed component) travel
// to the conformance corpus with the spec sentence that decides them — see
// the producer's corpus-candidates/.
//
// per §9.7, §28.3.8 (v0.359)

#include <string>

#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// The §9.7 / §28.3.8 diagnostic for a capture-bearing function value stored in
// a record's function-typed component.
static const char* kFnComponentStore =
    "a capture-bearing function value may not be stored in the record "
    "component `f`";

// The spec's own words for the rule, which the diagnostic cites.
static const char* kRuleCite = "§9.7 / §28.3.8";

// ===========================================================================
// REJECT — the issue's own repro (the record escapes through a local), and
// its DIRECT twin, which keeps exactly ONE diagnostic.
//
// The direct form (`return new R(lambda() => t.v);`) was already rejected on
// the base commit, by the §28.3 escape check that descends into the
// construction's arguments ("returned closure captures `t` whose source is
// disposed at scope end"). The fix rejects the STORE, so the direct form is
// now rejected twice over — but the program must keep the ONE diagnostic it
// has always had: the escape check reports first and the store leg stands
// down, so no second report appears. This row pins that (a) the direct form
// is still rejected, and (b) its diagnostic is unchanged and not duplicated.
//
// The direct-form assertions were a row of their own
// (`direct_return_form_keeps_its_single_existing_diagnostic_9_7`); they are
// FOLDED IN HERE because that row was BASE-GREEN (the direct form is rejected
// on the base too) and the suite may keep at most two base-green rows
// (CLAUDE.md, *Test and oracle rules*). The assertions are unchanged.
// ===========================================================================
BEZEL_TEST(record_fn_component_capture_free_8852,
           local_and_direct_return_of_record_over_owned_capture_rejected_9_7) {
    // The LOCAL-return form — the issue's own repro.
    // MEASURED before the fix: compiled clean (rc 0, two warnings) and the
    // AOT+ASan run SEGVed in `lam$0` — the escaped closure read the captured
    // owned local after the method's scope end disposed it.
    std::string src =
        "module P; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "record R(f: () -> int); "
        "class H "
        "  public method M(): R "
        "    var t := new Cell(5); "
        "    var r := new R(lambda() => t.v); "
        "    return r; "
        "  end M; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var acc := 0; "
        "    for var i in 0..<200 do "
        "      var r := h.M(); acc := acc + r.f(); "
        "    end; "
        "    Console.WriteLine(acc); "
        "    Console.WriteLine(Cell.n); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFnComponentStore));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRuleCite));

    // The DIRECT-return twin — rejected, with its ONE pre-existing
    // diagnostic and no second report for the same shape.
    std::string direct =
        "module P; "
        "class Cell "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "record R(f: () -> int); "
        "class H "
        "  public method M(): R "
        "    var t := new Cell(5); "
        "    return new R(lambda() => t.v); "
        "  end M; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var r := h.M(); "
        "    Console.WriteLine(r.f()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto d = runAot(direct);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(d));
    // The pre-existing §28.3 escape diagnostic, unchanged.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        d, "returned closure captures `t` whose source is disposed at scope "
           "end"));
    // Exactly one report for that shape — the store leg stands down.
    BEZEL_EXPECT_TRUE(diagnosticMessageCount(
                          d, "returned closure captures `t` whose source is "
                             "disposed at scope end") == 1);
}

// The FIELD-STORE twin of the issue's repro: the record is stored in an
// instance field of `this`, so the closure outlives the method entirely.
// MEASURED before the fix: compiled clean, and the same use-after-dispose.
BEZEL_TEST(record_fn_component_capture_free_8852,
           field_store_of_record_over_owned_capture_rejected_9_7) {
    std::string src =
        "module P; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "record R(f: () -> int); "
        "class H "
        "  public r: R := new R(lambda() => 0); "
        "  public method M(): int "
        "    var t := new Cell(5); "
        "    r := new R(lambda() => t.v); "
        "    return 1; "
        "  end M; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var acc := 0; "
        "    for var i in 0..<200 do "
        "      acc := acc + h.M(); "
        "    end; "
        "    Console.WriteLine(acc); "
        "    Console.WriteLine(Cell.n); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFnComponentStore));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRuleCite));
}

// The `with(...)` twin — §9.7's "at construction and in `with`" names both
// stores. MEASURED before the fix: compiled clean.
BEZEL_TEST(record_fn_component_capture_free_8852,
           with_override_of_record_over_owned_capture_rejected_9_7) {
    std::string src =
        "module P; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "record R(f: () -> int); "
        "class H "
        "  public method M(): R "
        "    var t := new Cell(5); "
        "    var b0 := new R(lambda() => 0); "
        "    var r := b0 with (f := lambda() => t.v); "
        "    return r; "
        "  end M; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var acc := 0; "
        "    for var i in 0..<200 do "
        "      var r := h.M(); acc := acc + r.f(); "
        "    end; "
        "    Console.WriteLine(acc); "
        "    Console.WriteLine(Cell.n); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFnComponentStore));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRuleCite));
}

// The RECEIVER-capture twin — §9.4's leg of the source set: a closure that
// captures the receiver has a non-empty set too, and the function-typed FIELD
// store already rejects it (the #6630 leg). The record COMPONENT did not.
// MEASURED before the fix: compiled clean.
BEZEL_TEST(record_fn_component_capture_free_8852,
           receiver_capture_into_record_component_rejected_9_7) {
    std::string src =
        "module P; "
        "class Cell "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "record R(f: () -> int); "
        "class H "
        "  public owned c: Cell := new Cell(3); "
        "  public method M(): R "
        "    var r := new R(lambda() => this.c.v); "
        "    return r; "
        "  end M; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var r := h.M(); "
        "    Console.WriteLine(r.f()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFnComponentStore));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRuleCite));
}

// ===========================================================================
// BOUNDARY CONTROL 1 — a CAPTURE-FREE function value is still admitted into
// a function-typed component.
//
// §9.7's rule admits exactly one value: "a capture-free function value". A
// lambda literal with no capture, and a lambda capturing a VALUE-typed local
// (a value type holds no borrows, §3.6), are both capture-free — so both are
// admitted at the construction and in `with`. The stdout assertion pins the
// VALUE, not only the absence of a diagnostic: a silent zero-fill would
// exit 0 too.
// ===========================================================================
BEZEL_TEST(record_fn_component_capture_free_8852,
           capture_free_function_value_still_admitted_into_component_9_7) {
    std::string src =
        "module P; "
        "import System; "
        "record R(f: () -> int); "
        "public class Program "
        "  public static method Main(): int "
        "    var k := 7; "
        "    var a := new R(lambda() => 42); "
        "    var b := new R(lambda() => k); "
        "    var c := a with (f := lambda() => k + 1); "
        "    Console.WriteLine(a.f() + b.f() + c.f()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(!failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 42 + 7 + 8 — the three components' values, read back through the
    // component.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("57\n"));
}

// ===========================================================================
// BOUNDARY CONTROL 2 — a call that resolved to a DECLARED constructor is not
// a call of the generated primary, so §28.3.8's store site does not fire
// there.
//
// §28.3.8 (v0.359), *Function-typed holders*: the stores are "an argument
// for a record component at **every call of the record's generated primary
// constructor** (§9.1) and a `with` assignment to it (§9.5)"; and "a
// record's declared constructor is checked like any other body, its plain
// function-typed parameter being call-bounded" (§28.14). §9.3 (v0.359): "A
// body constructor with a different parameter list is not required to chain
// to the primary" — a DECLARED constructor may stand beside the generated
// primary.
//
// So over
//
//   record P3(g: () -> int, n: int)
//     public constructor(cb: () -> int) ...
//
// the call `new P3(lambda() => t.v)` binds its argument to the DECLARED
// constructor's parameter `cb` — call-bounded, never stored, invoked inside
// the constructor — and NOT to the component `g`. The store rule does not
// fire at the call.
//
// The two programs are the `a24` (a plain declared parameter) and `a23` (a
// `scoped` declared parameter) shapes; they are FOLDED INTO ONE ROW because
// each is BASE-GREEN (the base admits both) and the suite may keep at most
// two base-green rows (CLAUDE.md, *Test and oracle rules*).
//
// The stdout assertion pins the VALUE: each program prints 200 (the
// component's capture-free closure, invoked 200 times) and 0 (no leaked
// cell).
// ===========================================================================
BEZEL_TEST(record_fn_component_capture_free_8852,
           declared_constructor_call_admits_capturing_argument_9_7) {
    // The PLAIN declared parameter — §28.14's call-bounded tier.
    std::string plain = R"BZL(
module P;
import System;
class Cell
  public static n: int := 0;
  public v: int;
  public constructor(x: int) this.v := x; Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
record R(f: () -> int);
record P3(g: () -> int, n: int)
  public constructor(cb: () -> int)
    this.g := lambda() => 1; this.n := cb();
  end constructor;
end P3;
class H
  public method M(): R
    var t := new Cell(5);
    var p := new P3(lambda() => t.v);
    return new R(p.g);
  end M;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      var r := h.M();
      acc := acc + r.f();
    end;
    Console.WriteLine(acc);
    Console.WriteLine(Cell.n);
    return 0;
  end Main;
end Program;
)BZL";
    auto r = runAot(plain);
    BEZEL_EXPECT_TRUE(!failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n0\n"));

    // The `scoped` declared parameter — §28.14's storage-bounded tier, whose
    // admission is the one the `scoped` spelling exists for.
    std::string scoped = R"BZL(
module P;
import System;
class Cell
  public static n: int := 0;
  public v: int;
  public constructor(x: int) this.v := x; Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
record R(f: () -> int);
record P3(g: () -> int, n: int)
  public constructor(scoped cb: () -> int)
    this.g := lambda() => 1; this.n := cb();
  end constructor;
end P3;
class H
  public method M(): R
    var t := new Cell(5);
    using var p := new P3(lambda() => t.v);
    return new R(p.g);
  end M;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      var r := h.M();
      acc := acc + r.f();
    end;
    Console.WriteLine(acc);
    Console.WriteLine(Cell.n);
    return 0;
  end Main;
end Program;
)BZL";
    auto s = runAot(scoped);
    BEZEL_EXPECT_TRUE(!failedAtCompileTimeWithRealLocation(s));
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, std::string("200\n0\n"));
}
