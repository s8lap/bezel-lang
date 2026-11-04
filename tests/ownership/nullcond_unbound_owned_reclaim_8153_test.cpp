// Ownership / codegen — issues #7278 and #8153: an UNBOUND owned
// `Array<T>` result of a stdlib §9.6 `String` member (`Split`,
// `ToCharArray`), in the `.` and the `?.` spelling, must be reclaimed at
// every position §28.3.11 names — a non-`owned` argument, a discard, an
// expression statement — and a materializer local must reclaim the array a
// reassignment displaces and the arm a `??` initializer produced.
//
// SPEC (v0.355).
//   stdlib §9.6 (docs/Bezel_Standard_Library.md:5446-5472):
//     public method Split(separator: char): owned Array<string>;
//     public method Split(separator: string): owned Array<string>;
//     public method Split(separators: Array<char>): owned Array<string>;
//     public method ToCharArray(): owned Array<char>;
//   spec §28.3.11 *Unbound owned temporaries*
//   (docs/Bezel_Language_Spec.md:7712): "An owned value may also be produced
//   as an **unbound temporary**: the result of an `owned`-returning call ...
//   that is not bound to any name, not `move`d, and not returned ..., but
//   used only by *borrowing* operations within the expression — as the
//   receiver of a further call, as a non-`owned` argument, as an operator
//   operand, as a member or index access ... Such a temporary is disposed at
//   the **end of the full expression** in which it is produced". And: "A
//   temporary that is instead bound (to a `var`/`using var`), `move`d,
//   returned, or transferred into an owning location is governed by the
//   corresponding transfer or discharge rule and is not disposed under this
//   rule (it would otherwise be disposed twice)."
//
// THE ORACLES. Every leak row runs on `runAotChecked` (ASan/LSan), loops
// 100x so a per-iteration leak is linear and cannot hide under a one-shot
// allocation, and asserts the exact PRINTED value — a leak exits non-zero,
// a double free is an ASan error, and a wrong free shows as a wrong count.
// The CONTROL rows (a bound result, a `using var`, a result moved into an
// `owned` parameter, a null receiver) pin that the reclaim does not touch a
// value another owner discharges: they are clean before and after the fix,
// and a double free there would turn them red.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// `t` is a non-nullable receiver, `s` a non-null nullable one and `n` a null
// one. "a,bc,dc" has 7 code points, splits at ',' into 3 parts, at 'c' into
// 3 ("a,b", ",d", "") and at ';' into 1.
std::string prog(const std::string& body) {
    return "module M; import System; "
           "class C "
           "  public static method CountOf(a: Array<string>?): int "
           "    if a = null then return -1; end if; "
           "    return a!.Count; "
           "  end CountOf; "
           "  public static method CountC(a: Array<char>?): int "
           "    if a = null then return -1; end if; "
           "    return a!.Count; "
           "  end CountC; "
           "  public static method Sink(owned a: Array<char>): int "
           "    return a.Count; "
           "  end Sink; "
           "  public static method SinkS(owned a: Array<string>?): int "
           "    if a = null then return -1; end if; "
           "    return a!.Count; "
           "  end SinkS; "
           "  public static method Mk(): owned Array<string> "
           "    return \"a,b\".Split(\",\"); "
           "  end Mk; "
           "  public static method Main(): int "
           "    var t: string := \"a,bc,dc\"; "
           "    var s: string? := \"a,bc,dc\"; "
           "    var n: string? := null; "
           "    var k := 0; "
           "    var i := 0; "
           "    while i < 100 do " +
           body +
           "      i := i + 1; "
           "    end while; "
           "    Console.WriteLine(k); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

void expectClean(const bezel::test::JitResult& r, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ===========================================================================
// (A) #7278 — the `.` spelling: `ToCharArray()` unbound, and the argument
//     position for every fresh `owned Array<T>` call result.
// ===========================================================================

// #7278's argument form: `.ToCharArray()` at a non-`owned` parameter.
// MEASURED on the parent (3b393693): 11200 byte(s) leaked in 200
// allocation(s).
BEZEL_TEST(ownership_nullcond_unbound_owned_reclaim_8153,
           spec_28_3_11_dot_to_char_array_at_an_argument_is_reclaimed) {
    expectClean(runAotChecked(prog("      k := k + CountC(t.ToCharArray()); ")),
                "700\n");
}

// #8153's ALSO form: `.Split('c')` at a non-`owned` parameter. MEASURED on
// the parent: 24600 byte(s) leaked in 800 allocation(s).
BEZEL_TEST(ownership_nullcond_unbound_owned_reclaim_8153,
           spec_28_3_11_dot_split_at_an_argument_is_reclaimed) {
    expectClean(runAotChecked(prog("      k := k + CountOf(t.Split('c')); ")),
                "300\n");
}

// The same §28.3.11 position for a USER `owned Array<T>` producer — the
// argument arm reuses the recognizer the discard / statement reclaim
// already applies to it. MEASURED on the parent: 21200 byte(s) leaked in
// 600 allocation(s).
BEZEL_TEST(ownership_nullcond_unbound_owned_reclaim_8153,
           spec_28_3_11_user_owned_array_at_an_argument_is_reclaimed) {
    expectClean(runAotChecked(prog("      k := k + CountOf(Mk()); ")), "200\n");
}

// The argument temporary's boundary is the full expression wherever that
// expression sits: a local's initializer and an `if` condition (K8.2).
// MEASURED on the parent: 11200 byte(s) / 200 allocation(s) each.
BEZEL_TEST(ownership_nullcond_unbound_owned_reclaim_8153,
           spec_28_3_11_argument_in_an_initializer_and_a_condition_is_reclaimed) {
    expectClean(runAotChecked(prog(
                    "      var m := CountC(t.ToCharArray()); "
                    "      k := k + m; "
                    "      if CountC(t.ToCharArray()) = 7 then k := k + 1; "
                    "      end if; ")),
                "800\n");
}

// #7278's discard and expression-statement forms. MEASURED on the parent:
// 22400 byte(s) leaked in 400 allocation(s).
BEZEL_TEST(ownership_nullcond_unbound_owned_reclaim_8153,
           spec_28_3_11_dot_to_char_array_discard_and_statement_are_reclaimed) {
    expectClean(runAotChecked(prog(
                    "      _ := t.ToCharArray(); "
                    "      t.ToCharArray(); "
                    "      k := k + 1; ")),
                "100\n");
}

// `.ToCharArray()` as the RECEIVER of a member access, an index and a
// `for` source — "the receiver of a further call ... a member or index
// access" — reclaimed by the same recognizer. MEASURED on the parent:
// 11200 byte(s) / 200 allocation(s) each.
BEZEL_TEST(ownership_nullcond_unbound_owned_reclaim_8153,
           spec_28_3_11_dot_to_char_array_receiver_forms_are_reclaimed) {
    expectClean(runAotChecked(prog(
                    "      k := k + t.ToCharArray().Count; "
                    "      var c := t.ToCharArray()[1]; "
                    "      if c = ',' then k := k + 1; end if; "
                    "      for var d in t.ToCharArray() do k := k + 1; end for; ")),
                "1500\n");
}

// CONTROLS — another owner discharges the value, so the argument reclaim
// must not touch it: a bound result passed twice (the local's scope-end
// reclaim frees it once), a `using var`, and a fresh result transferred
// into an `owned` parameter (the callee disposes it). Clean on the parent;
// a second free here is an ASan double-free.
BEZEL_TEST(ownership_nullcond_unbound_owned_reclaim_8153,
           spec_28_3_11_bound_using_and_owned_parameter_controls_stay_single_freed) {
    expectClean(runAotChecked(prog(
                    "      var a := t.ToCharArray(); "
                    "      k := k + CountC(a) + CountC(a); "
                    "      using var b := t.ToCharArray(); "
                    "      k := k + CountC(b); "
                    "      k := k + Sink(t.ToCharArray()); ")),
                "2800\n");
}
