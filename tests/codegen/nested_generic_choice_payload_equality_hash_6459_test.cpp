// Codegen — #6459: a §35.7 case payload that spells a type parameter BURIED
// INSIDE another generic's argument list (`Wp(v: W<T>)` at `VQ<Col>`) was left
// UNSUBSTITUTED, so `emitChoiceEquality` / `emitChoiceHashCode` /
// `emitChoiceToString` all fell to their SHAPE fallbacks and folded every slot
// of the choice struct in slot order — including the INACTIVE case's payload
// slots, which are `undef` in a freshly constructed value (the hazard #2083's
// own header documents).
//
// That is an uninitialised READ, and it is measurable: before the fix ONE
// executable printed `ha=573780 hb=573315` on one run, `hb=573377` on the next
// and `hb=575299` on a third, with `a = b` flipping alongside. So `=` answered
// False for two structurally equal values AND `GetHashCode` disagreed with it
// — §8.3's "`a.Equals(b)` implies `a.GetHashCode() = b.GetHashCode()`" broken
// on both halves.
//
// The consequence is not a wrong answer to a question but a container that
// silently loses entries (§8.4 / §8.3's `Set`/`Dictionary` paths go through the
// same fold) — hence the container rows below, which assert the PRINTED result
// rather than the exit code alone.
//
// Every row is compiled and RUN and asserts concrete VALUES. The defect's
// whole character is "exits 0 with a wrong answer", so an `exitCode == 0`
// oracle would be a vacuous green here; each body prints its observables and
// each row asserts the exact expected text.
//
// Not #6169 / #6458: those fixed a payload whose OWN spelling was `T?` losing
// its `?` at the payload site. This is a payload whose type argument is never
// substituted at all — a different substitution gap at the same two/three call
// sites, which is why it survived both of those fixes.

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `Col` is a value-like auto-hashable choice (§9.2's admissible list) with a
// payload-bearing case, so the pre-fix inactive-slot read is actually reached.
// `W<T>` spells the nullable payload of #6169 (`Ok(v: T?)`) and `VQ<T>` spells
// THIS issue's shape — a parameter buried inside another generic's args.
// `W2`/`VQ2` are the non-generic control the issue names (correct before the
// fix); `VConcrete<T>` takes the inner argument CONCRETELY (`W<Col>`), which is
// the discriminating spelling: it too was correct before the fix, and a
// "compare every slot" sentinel would leave it green. `V1`/`V2`/`V3` add two
// more layers of the same nesting for the depth row.
const char* const kPrelude = R"(module M;
choice Col Red, Rgb(v: int); end Col;
choice W<T> Ok(v: T?), No; end W;
choice VQ<T> Wp(v: W<T>); end VQ;
choice V1<T> A(v: W<T>); end V1;
choice V2<T> B(v: V1<T>); end V2;
choice V3<T> C(v: V2<T>); end V3;
choice W2 Ok(v: Col?), No; end W2;
choice VQ2 Wp(v: W2); end VQ2;
choice VConcrete<T> Wp(v: W<Col>); end VConcrete;
class Program
)";

// Body runs in Main; `return 1` on success. `extraMembers` are spliced in
// before `Main` (used by the separate-frame row).
std::string program(const std::string& body,
                    const std::string& extraMembers = std::string()) {
    return std::string(kPrelude) + extraMembers +
           "  public static method Main(): int\n" +
           body + "    return 1;\n  end Main;\nend Program;\n";
}

// Assert the program ran to completion AND its stdout is EXACTLY `expected`.
// Exact-match (not substring) so a program printing the right line among wrong
// ones cannot pass, and so a hash that varies cannot pass either — which is
// what the defect actually did. Only used at file scope inside the tests
// below, where `r` is in scope.
#define EXPECT_OUTPUT(r, expected)                                          \
    do {                                                                    \
        BEZEL_EXPECT_TRUE((r).ran);                                        \
        std::string got_ = (r).stdoutText;                                 \
        while (!got_.empty() &&                                            \
               (got_.back() == '\n' || got_.back() == '\r'))               \
            got_.pop_back();                                                \
        BEZEL_EXPECT_EQ(got_, std::string(expected));                      \
    } while (false)

}  // namespace

// §8.3 / §8.4 — THE PIN, equality half. Two structurally equal `VQ<Col>` values
// must compare equal.
//
// The two values are built in SEPARATE call frames, which is what makes this
// row DETERMINISTICALLY red without the fix rather than red only on some runs.
// Two constructions in one frame leave the same `undef` bytes in the inactive
// payload slots, so the pre-fix slot-order fold can agree by luck; two frames
// leave different garbage there. MEASURED pre-fix: this exact program printed
// `False` 12 times out of 12 runs of one executable, while the same-frame
// spelling printed `eq=False` on some runs and `True` on others — the
// nondeterminism the issue's "uninitialised slot" clue pointed at.
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           equal_nested_generic_choice_values_compare_equal_spec_8_3) {
    JitResult r = runAot(
        program("    var a := Program.Mk();\n"
                "    var b := Program.Mk();\n"
                "    Console.WriteLine($\"{a = b}\");\n",
                "  private static method Mk(): VQ<Col>\n"
                "    var p: Col? := Col.Rgb(1);\n"
                "    return VQ<Col>.Wp(W<Col>.Ok(p));\n"
                "  end Mk;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "True");
}

// §8.3 — the same two separate-frame values, this time reading the HASH half.
// Before the fix this printed `True 573315 573315` where 573315 is the fold
// over the inactive `undef` slots; with the substitution the fold is over the
// active payload only and both hashes are the correct 49043. Asserting the
// VALUE (not merely `ha = hb`) is what makes this row red pre-fix: a fold that
// agreed with itself on garbage would still pass an equality-only assertion.
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           separate_frame_constructions_hash_to_the_same_correct_value_spec_8_3) {
    JitResult r = runAotChecked(
        program("    var a := Program.Mk();\n"
                "    var b := Program.Mk();\n"
                "    Console.WriteLine($\"{a = b} {a.GetHashCode()} "
                "{b.GetHashCode()}\");\n",
                "  private static method Mk(): VQ<Col>\n"
                "    var p: Col? := Col.Rgb(1);\n"
                "    return VQ<Col>.Wp(W<Col>.Ok(p));\n"
                "  end Mk;\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "True 49043 49043");
}

// §8.3 — THE PIN, hash half, as §8.3's invariant states it. The two hashes
// must AGREE, and they must be the same value across the whole batch. Before
// the fix `hb` was 573315 / 573377 / 575299 on consecutive runs of one
// executable, so an exact-value assertion is what makes this row red: the
// value 49043 is the correctly-folded hash of this value at this shape.
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           hash_of_equal_nested_generic_choice_values_agree_spec_8_3) {
    JitResult r = runAot(program(
        "    var p: Col? := Col.Rgb(1);\n"
        "    var a := VQ<Col>.Wp(W<Col>.Ok(p));\n"
        "    var b := VQ<Col>.Wp(W<Col>.Ok(p));\n"
        "    Console.WriteLine($\"{a.GetHashCode()} {b.GetHashCode()}\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "49043 49043");
}

// §8.3 — the invariant in the form the issue reports it: `a = b` AND equal
// hashes, read in ONE program, so a fix that made only one half agree is red.
// Run through `runAotChecked` so the emitted IR is verified too.
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           equality_and_hash_agree_together_spec_8_3_hash_equality_invariant) {
    JitResult r = runAotChecked(program(
        "    var p: Col? := Col.Rgb(1);\n"
        "    var a := VQ<Col>.Wp(W<Col>.Ok(p));\n"
        "    var b := VQ<Col>.Wp(W<Col>.Ok(p));\n"
        "    Console.WriteLine($\"{a = b} {a.GetHashCode() = b.GetHashCode()}\");\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "True True");
}

// §8.3 — the DISCRIMINATING half: this row is what stops the fix being
// "make everything equal". Distinct payloads must stay unequal, and a
// null payload must stay unequal to a present one (§8.4's null-safe `=`). A
// fold that compared every slot, or that dropped the tag check, would print
// `True` here. Before the fix these were accidental (undef happened to match),
// not decided — the values below are the spec's answer, not the old ones.
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           distinct_nested_payloads_stay_unequal_spec_8_4) {
    JitResult r = runAot(program(
        "    var p: Col? := Col.Rgb(1);\n"
        "    var q: Col? := Col.Rgb(2);\n"
        "    var pn: Col? := null;\n"
        "    var a := VQ<Col>.Wp(W<Col>.Ok(p));\n"
        "    var b := VQ<Col>.Wp(W<Col>.Ok(q));\n"
        "    var n := VQ<Col>.Wp(W<Col>.Ok(pn));\n"
        "    Console.WriteLine($\"{a = b} {a = n} {n = a}\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "False False False");
}

// §35.7 — a payload-free case still compares by TAG ALONE at the nested level,
// and differs from the same case carrying a payload. This is the row a
// "compare every slot" sentinel breaks on one side and a "drop the tag check"
// sentinel on the other. Also asserts the payload-free value's hash is stable
// (16865 = the tag-only fold of `W<Col>.No` inside `VQ<Col>.Wp`).
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           nested_payload_free_case_compares_by_tag_alone_spec_35_7) {
    JitResult r = runAot(program(
        "    var a := VQ<Col>.Wp(W<Col>.No);\n"
        "    var b := VQ<Col>.Wp(W<Col>.No);\n"
        "    Console.WriteLine($\"{a = b} {a.GetHashCode()}\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "True 16865");
}

// §8.3 / §8.4 — THE CONSEQUENCE the issue names: a container keyed on such a
// value must not lose it. Before the fix the `Set`/`Dictionary` lookup ran
// through the same broken fold, so a value the container HELD reported itself
// absent. The lookup key is built in a SEPARATE call frame for the reason the
// equality pin gives: same-frame constructions leave matching `undef` bytes
// and the lookup can succeed by luck. MEASURED pre-fix, the separate-frame
// spelling printed `1 False 1 False 7` — count 1 (the value IS in there),
// `Contains` False, `ContainsKey` False — and the indexing `d[...]` THREW
// `KeyNotFoundException` on some runs of the same binary.
//
// Asserting the PRINTED values is the point: this defect's symptom is a wrong
// answer, and a bare `exitCode == 0` here would be a vacuous green.
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           set_and_dictionary_do_not_lose_a_nested_generic_choice_key_spec_8_3) {
    JitResult r = runAotChecked(
        program("    using var s := new Set<VQ<Col>>();\n"
                "    using var d := new Dictionary<VQ<Col>, int>();\n"
                "    var a := Program.Mk();\n"
                "    s.Add(a);\n"
                "    d.Add(a, 7);\n"
                "    Console.WriteLine($\"{s.Count} {s.Contains(Program.Mk())} "
                "{d.Count} {d.ContainsKey(Program.Mk())} "
                "{d[Program.Mk()]}\");\n",
                "  private static method Mk(): VQ<Col>\n"
                "    var p: Col? := Col.Rgb(1);\n"
                "    return VQ<Col>.Wp(W<Col>.Ok(p));\n"
                "  end Mk;\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "1 True 1 True 7");
}

// §8.5 / §35.7 — the third §35.7 consumer, and the row that shows the defect
// was never a comparison-only problem: the SAME substitution gap left the
// rendering's payload slot BLANK. `Col` has cases Red (payload-free) and
// Rgb(v: int), so before the fix the rendering dropped the nested case's name,
// its payload name and its value. Asserting the exact text makes a silent
// degradation red rather than merely ugly.
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           to_string_renders_the_nested_generic_payload_spec_8_5) {
    JitResult r = runAot(program(
        "    var p: Col? := Col.Rgb(1);\n"
        "    var a := VQ<Col>.Wp(W<Col>.Ok(p));\n"
        "    Console.WriteLine($\"{a}\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "VQ.Wp(v: W.Ok(v: Col.Rgb(v: 1)))");
}

// §14.1 — the substitution composes through ARBITRARY depth, not one level.
// Three nested generic layers each carrying a parameter inside the next one's
// arguments. Before the fix every layer after the first fell to its shape
// fallback. This is the row that would stay red after a one-level fix.
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           substitution_composes_through_three_levels_of_nesting_spec_14_1) {
    JitResult r = runAot(program(
        "    var p: Col? := Col.Rgb(1);\n"
        "    var a := V3<Col>.C(V2<Col>.B(V1<Col>.A(W<Col>.Ok(p))));\n"
        "    var b := V3<Col>.C(V2<Col>.B(V1<Col>.A(W<Col>.Ok(p))));\n"
        "    Console.WriteLine($\"{a = b} {a.GetHashCode()} {b.GetHashCode()}\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "True 81717 81717");
}

// §4.6 / §8.3 — the CONTENT-equality consequence at the nested level, which is
// what #4278 fixed for the one-level shape. A `string` payload must compare by
// CONTENT at any depth; a fold that compared the payload POINTERS would make
// `"hi" = "hi"` False here. This is the row that distinguishes the fix from a
// narrower one that only handled the choice-typed payload.
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           nested_string_payload_compares_by_content_spec_4_6) {
    JitResult r = runAot(program(
        "    var a := VQ<string>.Wp(W<string>.Ok(\"hi\"));\n"
        "    var b := VQ<string>.Wp(W<string>.Ok(\"hi\"));\n"
        "    var c := VQ<string>.Wp(W<string>.Ok(\"bye\"));\n"
        "    Console.WriteLine($\"{a = b} {a = c} "
        "{a.GetHashCode() = b.GetHashCode()}\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "True False True");
}

// §35.7 — THE CONTROLS the issue names, as rows rather than as prose. The
// non-generic spelling (`W2`/`VQ2`, no `T` anywhere) and the spelling that
// takes the inner argument CONCRETELY (`VConcrete<T>`) were BOTH correct
// before the fix and must STAY correct — these are the rows a change to the
// shared payload resolution would break if it over-reached. Asserted as
// VALUES, so a regression to the shape fallback is red here too.
BEZEL_TEST(nested_generic_choice_payload_equality_hash_6459,
           non_generic_and_concrete_inner_argument_controls_stay_correct_spec_35_7) {
    JitResult r = runAot(program(
        "    var p: Col? := Col.Rgb(1);\n"
        "    var n1 := VQ2.Wp(W2.Ok(p));\n"
        "    var n2 := VQ2.Wp(W2.Ok(p));\n"
        "    var c1 := VConcrete<Col>.Wp(W<Col>.Ok(p));\n"
        "    var c2 := VConcrete<Col>.Wp(W<Col>.Ok(p));\n"
        "    Console.WriteLine($\"{n1 = n2} {n1.GetHashCode()} "
        "{c1 = c2} {c1.GetHashCode()} {c2.GetHashCode()}\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    EXPECT_OUTPUT(r, "True 49043 True 49043 49043");
}