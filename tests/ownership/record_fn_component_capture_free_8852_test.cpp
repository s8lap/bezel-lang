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
// REJECT — the issue's own repro (the record escapes through a local).
// ===========================================================================

// MEASURED before the fix: compiled clean (rc 0, two warnings) and the
// AOT+ASan run SEGVed in `lam$0` — the escaped closure read the captured
// owned local after the method's scope end disposed it.
BEZEL_TEST(record_fn_component_capture_free_8852,
           local_return_of_record_over_owned_capture_rejected_9_7) {
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
// BOUNDARY CONTROL 1 — the DIRECT form keeps its EXISTING diagnostic, and
// keeps exactly ONE.
//
// `return new R(lambda() => t.v);` was already rejected on the base commit,
// by the §28.3 escape check that descends into the construction's arguments
// ("returned closure captures `t` whose source is disposed at scope end").
// The fix rejects the STORE, so the direct form is now rejected twice over —
// but the program must keep the ONE diagnostic it has always had: the escape
// check reports first and the store leg stands down, so no second report
// appears. This control pins that (a) the direct form is still rejected, and
// (b) its diagnostic is unchanged and not duplicated.
// ===========================================================================
BEZEL_TEST(record_fn_component_capture_free_8852,
           direct_return_form_keeps_its_single_existing_diagnostic_9_7) {
    std::string src =
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
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The pre-existing §28.3 escape diagnostic, unchanged.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "returned closure captures `t` whose source is disposed at scope "
           "end"));
    // Exactly one report for that shape — the store leg stands down.
    BEZEL_EXPECT_TRUE(diagnosticMessageCount(
                          r, "returned closure captures `t` whose source is "
                             "disposed at scope end") == 1);
}

// ===========================================================================
// BOUNDARY CONTROL 2 — a CAPTURE-FREE function value is still admitted into
// a function-typed component.
//
// §9.7's rule admits exactly one value: "a capture-free function value". A
// lambda literal with no capture, and a lambda capturing a VALUE-typed local
// (a value type holds no borrows, §3.6), are both capture-free — so both are
// admitted at the construction and in `with`.
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
}
