// Ownership — an UNMARKED mint-announcing ITERABLE return is an OWNED frame
// at the call site (issue #7762; spec §28.3 / §28.3.5 / §28.3.6 / §28.1 /
// §29.3 / §30.7, v0.355).
//
// THE RULE. §28.3.6 *Calls mint for the mint-announcing families*:
//
//   clause 1: "A call whose declared return type — read from the signature
//   *before* generic substitution, with nullability stripped (`Task?`,
//   `IEnumerable<T>?` qualify) — is a mint-announcing type produces an owned
//   frame at the call site, at every callee kind — the announcement table's
//   rows 2–3, all four columns".
//   clause 2: "The mint-announcing types are the **`Task` family** (`Task`,
//   `Task<T>`, `OwningTask<T>`) and the **iterable family** — the §29.3/§30.7
//   iterable and enumerator interfaces (`IEnumerable<T>`, `IEnumerator<T>`,
//   `IAsyncEnumerable<T>`, `IAsyncEnumerator<T>`, `IOwningEnumerable<T>`,
//   `IOwningEnumerator<T>`, `IOwningAsyncEnumerable<T>`,
//   `IOwningAsyncEnumerator<T>`) together with the lazy query".
//
// §28.1 adds: on a mint-announcing return type "the marker announces nothing
// the declared type has not already announced: **either spelling matches, in
// either direction**" — the announcement table's rows 2 and 3 are identical on
// both the caller and the callee side. So `owned IEnumerable<int>?` and the
// unmarked `IEnumerable<int>?` must classify IDENTICALLY at the call site.
//
// THE DEFECT (#7762). The calls-mint set named only the `Task` family
// (SB01-A). The four NON-OWNING iterable heads matched no calls-mint row at
// all, so `var e := h.MkU();` bound a BORROW and `s.Take(move e)` was refused
// with §28.8's "a borrow cannot be passed where an `owned` value is required" —
// while the MARKED spelling of the very same mint compiled. (The four OWNING
// iterable heads were already Owned, via the weaker `isOwningTypeHead`
// fallback that keys on the RESOLVED RESULT type.)
//
// WHAT THIS FILE PINS.
//   (1) the unmarked mint, `move`d into an `owned` parameter, compiles and the
//       frame is disposed EXACTLY once (the `fins` / `ds` counters);
//   (2) the MARKED spelling is the control — same counters, same verdicts;
//   (3) the BARE spelling (no `move`) is STILL REFUSED — the §22.3
//       missing-marker diagnostic, exactly as the MARKED spelling's bare form
//       is refused (the two spellings must not diverge);
//   (4) the two "do not widen the set" rows: a non-iterable, non-`Task`
//       unmarked reference type and a USER-DEFINED iterable-ish interface keep
//       the borrow classification and are still refused by §28.8;
//   (5) the `Task`-family twin — the model the iterable row mirrors.
//
// Every runtime row loops 50x under generated-code ASan/LSan (runAotChecked)
// and pins exact stdout.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// stdout (trailing newline stripped) of a clean, ASan/LSan-checked run, or a
// marker naming the failure (compile error, non-zero exit incl. a leak report).
std::string out(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// The mint source: `GenS()` yields 1, 2 and counts its own disposal in
// `H.fins`; `Mk()` is the MARKED mint (`owned IEnumerable<int>?`), `MkU()` the
// UNMARKED one (`IEnumerable<int>?`) — the two spellings of ONE
// mint-announcing return type.
const char* kPrelude = R"BZL(
module M;
public class W
  public static ds: int := 0;
  public v: int;
  public constructor(v: int) this.v := v; end constructor;
  destructor W.ds := W.ds + 1; end destructor;
end W;
public class H
  public static fins: int := 0;
  public constructor() end constructor;
  public method GenS(): owned IEnumerable<int>
    try yield return 1; yield return 2;
    finally H.fins := H.fins + 1; end;
  end GenS;
  public method Mk(): owned IEnumerable<int>?
    return this.GenS();
  end Mk;
  public method MkU(): IEnumerable<int>?
    return this.GenS();
  end MkU;
end H;
public class Sink
  public owned Held: IEnumerable<int>?;
  public constructor() end constructor;
  public method Take(owned e: IEnumerable<int>?) Held := e; end Take;
  public method SumHeld(): int
    var t := 0;
    for var x in Held! do t := t + x; end;
    return t;
  end SumHeld;
  public static method Count(e: IEnumerable<int>?): int
    var t := 0;
    for var x in e! do t := t + x; end;
    return t;
  end Count;
end Sink;
)BZL";

std::string loopProg(const std::string& body, int n = 50) {
    return std::string(kPrelude) +
           "public class C\n"
           "  public static method Main(): int\n"
           "    var sum := 0; var i := 0;\n"
           "    while i < " + std::to_string(n) + " do\n"
           "      using var h := new H();\n"
           "      using var s := new Sink();\n" +
           body +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine(\"sum=\" + sum.ToString() + \" fins=\" + "
           "H.fins.ToString() + \" ds=\" + W.ds.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A one-shot program: `body` sits in Main.
std::string oneShotProg(const std::string& body) {
    return std::string(kPrelude) +
           "public class C\n"
           "  public static method Main(): int\n"
           "    using var h := new H();\n"
           "    using var s := new Sink();\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// §22.3 (v0.169): a NAMED OWNED BINDING passed to an `owned` parameter leaves
// it moved-from, so the call site must announce the transfer with `move`.
const char* kMissingMarkerNeedle = "this call consumes `e`";
// §28.8: a BORROW flowing into an `owned` sink is rejected outright.
const char* kBorrowNeedle =
    "a borrow cannot be passed where an `owned` value is required";

void expectRejected(const std::string& src, const char* needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// --- (1) the UNMARKED iterable mint, `move`d into an `owned` parameter -------

BEZEL_TEST(unmarked_iterable_mint_move_7762,
           var_unmarked_iterable_mint_moved_into_owned_sink_50x) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var e := h.MkU(); s.Take(move e); "
                     "sum := sum + s.SumHeld();\n")),
        "sum=150 fins=50 ds=0");
}

// --- (2) the MARKED spelling is the CONTROL — identical counters -------------

BEZEL_TEST(unmarked_iterable_mint_move_7762,
           control_var_marked_iterable_mint_moved_into_owned_sink_50x) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var e := h.Mk(); s.Take(move e); "
                     "sum := sum + s.SumHeld();\n")),
        "sum=150 fins=50 ds=0");
}

// The 100-iteration shape of the issue's own repro, with the disposal
// counters: the frame is disposed EXACTLY once per iteration.
BEZEL_TEST(unmarked_iterable_mint_move_7762,
           issue_repro_shape_100x_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var e := h.MkU(); s.Take(move e); "
                     "sum := sum + s.SumHeld();\n", 100)),
        "sum=300 fins=100 ds=0");
}

// --- (3) the BARE spelling is STILL REFUSED (the marker is required) --------

// The unmarked mint's bare form: the value is now correctly OWNED, so the
// refusal is the §22.3 missing-marker diagnostic — NOT the §28.8 borrow
// diagnostic (the value is no longer a borrow). The MARKED spelling's bare
// form (the control, row 2 of the pair) refuses with the SAME diagnostic —
// §28.1's "either spelling matches, in either direction".
BEZEL_TEST(unmarked_iterable_mint_move_7762,
           bare_spelling_of_unmarked_mint_still_refused_missing_marker) {
    auto r = runAot(oneShotProg("    var e := h.MkU();\n"
                       "    s.Take(e);\n"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMissingMarkerNeedle));
    // The value is not a borrow any more, so §28.8's borrow refusal must NOT
    // fire: the two diagnostics are not interchangeable.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kBorrowNeedle));
}

BEZEL_TEST(unmarked_iterable_mint_move_7762,
           control_bare_spelling_of_marked_mint_refused_missing_marker) {
    auto r = runAot(oneShotProg("    var e := h.Mk();\n"
                       "    s.Take(e);\n"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMissingMarkerNeedle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kBorrowNeedle));
}

// --- (4) the minted frame is usable as a BORROW at a non-`owned` sink -------

BEZEL_TEST(unmarked_iterable_mint_move_7762,
           unmarked_mint_used_as_borrow_at_non_owned_sink) {
    auto r = runAot(oneShotProg("    var e := h.MkU();\n"
                       "    var n := Sink.Count(e);\n"
                       "    if n <> 3 then return 1; end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// --- (5) the `Task`-family twin — the model the iterable row mirrors --------

BEZEL_TEST(unmarked_iterable_mint_move_7762,
           task_family_unmarked_mint_moved_into_owned_sink) {
    auto r = runAot(
        "module M; "
        "public class H "
        "  public method MkT(): Task<int>? => Task.FromResult(0); "
        "end H; "
        "public class Sink "
        "  public method Take(owned t: Task<int>?) end Take; "
        "end Sink; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new H(); "
        "    using var s := new Sink(); "
        "    var t := h.MkT(); "
        "    s.Take(move t); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// --- (6) the "do not widen the set" rows: STILL REFUSED ---------------------

// A non-iterable, non-`Task` unmarked reference type is NOT mint-announcing:
// it keeps the borrow classification and §28.8 refuses the transfer.
BEZEL_TEST(unmarked_iterable_mint_move_7762,
           non_mint_unmarked_reference_type_still_refused) {
    expectRejected(
        "module M; "
        "public interface ICmp end ICmp; "
        "public class H "
        "  public method Get(): ICmp? return null; end Get; "
        "end H; "
        "public class Sink "
        "  public method Take(owned e: ICmp?) end Take; "
        "end Sink; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new H(); "
        "    using var s := new Sink(); "
        "    var e := h.Get(); "
        "    s.Take(move e); "
        "    return 0; "
        "  end Main; "
        "end C;",
        kBorrowNeedle);
}

// A USER-DEFINED iterable-ish interface is NOT one of the eight §29.3/§30.7
// heads: the mint-announcing set is a FIXED head list.
BEZEL_TEST(unmarked_iterable_mint_move_7762,
           user_defined_iterableish_interface_still_refused) {
    expectRejected(
        "module M; "
        "public interface ISequence end ISequence; "
        "public class H "
        "  public method Get(): ISequence? return null; end Get; "
        "end H; "
        "public class Sink "
        "  public method Take(owned e: ISequence?) end Take; "
        "end Sink; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new H(); "
        "    using var s := new Sink(); "
        "    var e := h.Get(); "
        "    s.Take(move e); "
        "    return 0; "
        "  end Main; "
        "end C;",
        kBorrowNeedle);
}

// --- (7) the `!` spelling (§17.8's null-forgiving over a fresh producer) ---

BEZEL_TEST(unmarked_iterable_mint_move_7762,
           bang_unmarked_iterable_mint_moved_into_owned_sink) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var e := h.MkU()!; s.Take(move e); "
                     "sum := sum + s.SumHeld();\n")),
        "sum=150 fins=50 ds=0");
}

// --- (8) `using var` over the unmarked mint (the campaign-1 shape) ----------

// §28.3.6 clause 4: "a `using var` over such a call **assumes the disposal
// obligation** (an owned right-hand side, not a §28.10 pin)". The frame is
// disposed exactly once, at scope end.
BEZEL_TEST(unmarked_iterable_mint_move_7762,
           using_var_over_unmarked_iterable_mint_disposed_once_100x) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      using var e := h.MkU(); "
                     "for var x in e! do sum := sum + x; end;\n", 100)),
        "sum=300 fins=100 ds=0");
}

// --- (9) an INSTANCE method with an unmarked iterable return MINTS ----------

// The companion of `deferred_activation_temp`'s
// `instance_method_receiver_borrow_not_flagged` row: that row's "receiver-
// sourced borrow" is now spelled at the CONCRETE collection type (the
// announcement table's row 4). This row pins the MINT-ANNOUNCING sibling —
// an instance method whose declared return type is a §29.3/§30.7 head mints
// an owned frame too (the announcement table's row 2, all four callee-kind
// columns), so §28.3's unbound-owned-temporary rule DOES fire when the frame
// is passed as a plain argument to a deferred-activation callee.
BEZEL_TEST(unmarked_iterable_mint_move_7762,
           instance_method_unmarked_iterable_return_mints_an_owned_frame) {
    expectRejected(
        "module M; "
        "class Holder "
        "  public method Items(): IEnumerable<int> "
        "    yield return 7; "
        "  end Items; "
        "end Holder; "
        "class Program "
        "  public static method Doubled(src: IEnumerable<int>): IEnumerable<int> "
        "    for var x in src do "
        "      yield return x * 2; "
        "    end; "
        "  end Doubled; "
        "  public static method M() "
        "    using var h := new Holder(); "
        "    for var v in Doubled(h.Items()) do end; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end Program;",
        "an unbound owned temporary passed as a plain argument to a "
        "deferred-activation callee");
}
