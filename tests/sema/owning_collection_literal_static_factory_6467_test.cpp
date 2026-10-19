// fix #6467 — §15.4.1 *Owning form* / *Recognition category* (v0.338), stdlib
// §4.5 (v0.338), spec §28.3.
//
// THE ISSUE'S PREMISE, AND WHY THIS FILE PINS THE OTHER VERDICT.
//
// Issue #6467 reports the §15.4.1 *Owning form* static call
// `OwningArray<T>.FromCollectionLiteral(owned xs)` as a compile-time
// SIGSEGV (exit 139) and prescribes a lowering fix — "the fix has to give
// the callee an owned-parameter transfer, which means giving the built-in a
// `MethodSymbol`-shaped call site (or an equivalent ownership-pass fact
// keyed on the carrier being `owned`)".
//
// MEASURED, that verdict is not reachable from any committed tree, and the
// defect that IS reachable is the opposite of the one reported:
//
//   * On `r30-c1` (this lane's base) the call is REJECTED — the false §3.4
//     `'OwningArray' has no static method or property 'FromCollectionLiteral'`.
//     No crash, no execution. Rows 1-4 below pin that verdict.
//   * On `978588d0` (`fix #6460`, branch r31-p-5983 — NOT an ancestor of this
//     base, so the decline it adds is absent here) the call is DECLINED WITH
//     REASONS: "a direct static 'OwningArray<...>.FromCollectionLiteral(owned
//     ...)' call is declared but not yet lowered ...".
//   * With that decline ARM DISABLED — i.e. the form fully admitted, which is
//     what the issue asks for — the program COMPILES, RUNS, and exits 0. It
//     does NOT double-free and does NOT segv. What it does is emit the §2434
//     degrade ("lowerLocalDecl's initializer expression failed to lower ...
//     degraded to a zero-initialized value"), print `0` where the spec's
//     element count is `1`, and LEAK the source array: 200 bytes in 3
//     allocations per iteration, MEASURED scaling linearly to 40,000 bytes
//     in 600 allocations over 200 iterations.
//
// So admitting the form is a SILENT-ACCEPTANCE INVERSION (compiles, exit 0,
// wrong answer, leaks) — the top defect class in this repository — and the
// issue's SIGSEGV/exit-139 framing is refuted. The decline is correct; this
// file pins it so that a future lowering arm cannot admit the form without
// also discharging its argument, which is precisely the property the leak row
// below measures.
//
// THE USER-TYPE CONTROL (row 5) IS WHAT MAKES THE ISSUE'S MECHANISM HALF
// RIGHT. §15.4.1's owning form over a USER class that conforms to
// `IOwningCollectionLiteralTarget` compiles and runs correctly under ASan on
// this very base — that callee HAS a `MethodSymbol`, so §28.3's
// owned-argument transfer fires and the obligation discharges. The built-in
// declares no method, which is exactly the asymmetry the issue identifies.
// The half that is wrong is the CONSEQUENCE: without that transfer the result
// is never the moved argument at all, so there is nothing to double-free;
// the argument is simply never discharged.
//
// per §15.4.1 (v0.338), stdlib §4.5, spec §28.3, spec §28.9.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The issue's literal repro, verbatim in shape: build an `OwningArray<Item>`,
// hand it to the §15.4.1 owning-form factory as a move, read the result's
// Count. `body` supplies the statement sequence up to the call.
const char* kPrelude =
    "module M; class Item public constructor() end constructor; end Item; "
    "class C public static method Main(): int ";

// True iff the program was rejected by the front end at a real source
// location (never a crash, hang, signal or ASan report) AND some diagnostic
// names the owning-form factory as undeclared-on-this-type. On this base the
// #6265 §3.4 guard is what fires, so the needle is the §3.4 text; when the
// #6460 decline is picked in, the same needle still matches because that
// message also names the member. Asserting the needle is what keeps the row
// from being vacuous — `failedAtCompileTimeWithRealLocation` alone would pass
// on ANY rejection, including an unrelated one.
bool owningFormRejected(const std::string& body) {
    auto r = runAot(kPrelude + body + " return 0; end Main; end C;");
    return bezel::test::failedAtCompileTimeWithRealLocation(r) &&
           bezel::test::diagnosticMessageContains(
               r, "'FromCollectionLiteral'") &&
           bezel::test::diagnosticMessageContains(r, "OwningArray");
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — the owning form is declared (stdlib §4.5) but is NOT admitted.
// None of these may ever reach execution.
// ---------------------------------------------------------------------------

BEZEL_TEST(owning_collection_literal_static_factory_6467,
           issue_repro_owning_form_move_argument_rejected) {
    // The issue's own repro. Before this row it was believed to SIGSEGV at
    // run time; it is rejected at compile time, before any code is emitted.
    BEZEL_EXPECT_TRUE(owningFormRejected(
        "var xs := new OwningArray<Item>(); xs.Add(new Item()); "
        "var ys := OwningArray<Item>.FromCollectionLiteral(move xs); "
        "Console.WriteLine(ys.Count); "));
}

BEZEL_TEST(owning_collection_literal_static_factory_6467,
           owning_form_bare_argument_rejected) {
    // The §28.3 marker is optional at the call site when the parameter is
    // `owned` (stdlib §4.5 declares `owned elements: OwningArray<T>`); the
    // rejection must not depend on the spelling at the call.
    BEZEL_EXPECT_TRUE(owningFormRejected(
        "var xs := new OwningArray<Item>(); xs.Add(new Item()); "
        "var ys := OwningArray<Item>.FromCollectionLiteral(xs); "
        "Console.WriteLine(ys.Count); "));
}

BEZEL_TEST(owning_collection_literal_static_factory_6467,
           owning_form_return_position_rejected) {
    // A return slot is a destination too (§28.11 enumerates them); the
    // rejection must not be reachable-around by placing the result
    // somewhere other than a binding.
    BEZEL_EXPECT_TRUE(owningFormRejected(
        "var xs := new OwningArray<Item>(); xs.Add(new Item()); "
        "return OwningArray<Item>.FromCollectionLiteral(move xs).Count; "));
}

BEZEL_TEST(owning_collection_literal_static_factory_6467,
           owning_form_temporary_argument_rejected) {
    // A fresh `new OwningArray<Item>()` carrier — the §28.3 unbound-owned-
    // temporary shape, which §15.4.1's *Lowering* says the compiler
    // materializes for a literal. Hand-written here, it is still the same
    // call and must carry the same verdict.
    BEZEL_EXPECT_TRUE(owningFormRejected(
        "var ys := OwningArray<Item>.FromCollectionLiteral("
        "new OwningArray<Item>()); Console.WriteLine(ys.Count); "));
}

// ---------------------------------------------------------------------------
// THE CONTROL THAT ISOLATES THE MECHANISM — the USER-type twin of the same
// §15.4.1 owning form compiles and runs, because that callee HAS a
// MethodSymbol. This is the row that makes rows 1-4 meaningful: it shows the
// shape is sound in general and that the built-in's missing callee
// representation is the whole difference.
// ---------------------------------------------------------------------------

BEZEL_TEST(owning_collection_literal_static_factory_6467,
           user_type_owning_form_with_methodsymbol_runs_clean) {
    // `Box<T>` conforms to §15.4.1's `IOwningCollectionLiteralTarget` and
    // supplies the `owned elements: OwningArray<T>` witness. §28.3's
    // owned-argument transfer fires on the real MethodSymbol, so the moved
    // array IS discharged and the result is sound.
    //
    // runAotChecked: an ownership/disposal row must be leak-checked, not
    // merely exit-checked (tests/README.md). Under LSan a non-discharged
    // array would flip `ran` to false.
    auto r = bezel::test::runAotChecked(
        "module M; class Item public constructor() end constructor; end Item; "
        "public class Box<T>: IOwningCollectionLiteralTarget<Box<T>, T> "
        "  where T: class "
        "  private owned _storage: OwningArray<T>; "
        "  public constructor() _storage := new OwningArray<T>(); "
        "    end constructor; "
        "  public static method FromCollectionLiteral("
        "      owned elements: OwningArray<T>): owned Box<T> "
        "    var b := new Box<T>(); b._storage := elements; return b; "
        "  end FromCollectionLiteral; "
        // An explicit METHOD, deliberately not an auto-property over the same
        // private owned field. MEASURED on this base: an auto-property here
        // reads 0 where this method reads 1, with no factory involved — filed
        // as #6472. Using the auto-property would make this row measure that
        // defect instead of the ownership transfer it exists to pin.
        "  public method Size(): int "
        "    return _storage.Count; "
        "  end Size; "
        "end Box; "
        "class C public static method Main(): int "
        "  var xs := new OwningArray<Item>(); xs.Add(new Item()); "
        "  using var ys := Box<Item>.FromCollectionLiteral(move xs); "
        "  Console.WriteLine(ys.Size()); "
        "  return 0; "
        "end Main; end C;");
    // MEASURED on this base: compiles, prints 1, ASan/LSan clean. The exit
    // code is 0 by construction (Main returns 0) — the `ran` assertion is
    // the load-bearing one, since a leak would clear it.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    // A stdout oracle, not the exit code: §15.4.1's element is ONE `Item`, so
    // a result that is anything other than the moved array reads 0 here. This
    // is the row that would catch a zero-initialized degrade (§2434) — the
    // exact failure mode MEASURED when the built-in owning form is admitted.
    //
    // `stdoutText` is the RAW capture, terminator included (only `runAotOut`
    // strips one), so the expectation carries the newline rather than
    // pretending the value is platform-independent.
    BEZEL_EXPECT_EQ(std::string("1\n"), r.stdoutText);
}