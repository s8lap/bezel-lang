// Codegen — #6465: an interpolation hole whose expression is a GENERIC CHOICE
// passed no instantiation to the choice renderer, so the payload resolved to
// the bare type parameter and rendered as the EMPTY string.
//
// The hole dispatch in `LlvmBackend_Exprs.cpp` (the struct-typed-hole arm)
// asked `isAutoEqualityBearingChoice` to classify AT `structRt` — the
// instantiation — and then called `emitChoiceToString(ev, ts)` with `instTy`
// left at its default of null. With no instantiation, the renderer took its
// `resolveTypeParamRef` else-arm, which returns its argument UNCHANGED when
// the active substitution has no entry for the head. MEASURED: at the hole
// the substitution is empty (the member-call path installs the receiver's own
// arguments, the hole path installs nothing), so a payload spelled `T`
// resolved to the type parameter itself, and `emitValueToString` rendered the
// empty string for it.
//
// MEASURED symptom, one program, one binary:
//   $"{W<Col>.Ok(pn)}"  ->  W.Ok(v: )
//   n.ToString()        ->  W.Ok(v: null)
// Same value, two answers. §35.7 fixes the rendering as
// "TypeName.CaseName(field1: value1, …)" with no component latitude, and §8.5
// makes each payload render as its own `ToString` renders it while §18.5 fixes
// the absent value's text as the four characters `null` — "Rendering `null`
// rather than the empty string keeps absent values diagnosable in output".
//
// This is the FOURTH spelling of the #6169 / #6458 payload-substitution
// defect class, at a different call site with a different MECHANISM: the other
// spellings dropped the declared `?` from a substitution that was performed,
// this one never substituted at all. It is a silently-wrong rendering, not a
// crash, which is why rows here assert the EXACT text on both legs rather
// than that the compiler survived.
//
// Every row pins the hole AND the `.ToString()` call of the SAME value from
// the SAME program, so a regression that makes the two disagree is red on the
// string compare rather than needing a separate judgment.

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `Col` is a value-like auto-hashable choice (§9.2's admissible list) with a
// payload-bearing case, so `Col?` is an admissible choice payload (§9.2 admits
// `T?` where `T` is admissible). `W<T>` gives its first case a payload spelled
// `T?` — the shape of the issue — so at `W<Col>` the payload is a nullable
// `Col`, whose absent rendering is `null`. `WN<T>` spells the same payload
// WITHOUT the `?`: the control showing the defect was never about the
// nullability flag (a PRESENT payload through `WN<Col>` was equally empty), and
// the control against a fix that marks a substituted payload nullable
// unconditionally. `WG<T>` is a generic choice whose only case is payload-free:
// the §35.7 payload-free row, which needs no instantiation and is therefore
// green on both legs of the fix. `R` is a record with a `W<Col>` component, so
// the hole over `R` reaches the renderer through `emitRecordToString` — the
// §8.5 component rule at a path that already passed its instantiation.
const char* const kPrelude = R"(module M;
choice Col Red, Rgb(v: int); end Col;
choice W<T> Ok(v: T?), No; end W;
choice WN<T> Ok(v: T), No; end WN;
choice WG<T> No; end WG;
record R(N: W<Col>);
class Program
)";

// Body runs in Main and returns 1 when every expectation held.
std::string program(const std::string& body) {
    return std::string(kPrelude) + "  public static method Main(): int\n" +
           body +
           R"(    return 1;
  end Main;
end Program;
)";
}

// Returns the program's stdout text (trailing newline included), so a row can
// pin the rendered string rather than merely that the compiler survived.
std::string stdoutOf(const JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// Splits stdout into lines with the terminators removed, so a row can compare
// the hole's rendering against the call's rendering as VALUES rather than only
// as a literal block. Returns an empty vector when the program did not run.
std::vector<std::string> linesOf(const JitResult& r) {
    std::vector<std::string> out;
    if (!r.ran) return out;
    std::string s = r.stdoutText;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

// §35.7 / §18.5: the issue's literal repro. The FIRST line of each pair is the
// interpolation hole — the spelling #6465 is about — and the second is the
// `.ToString()` call on the identical value, so the two renderings are pinned
// from one program and a divergence between them is a string-compare failure.
// Both a PRESENT and an ABSENT payload are covered, because before the fix
// they were wrong in the SAME way (both empty): the defect was the missing
// instantiation, not the absent value.
BEZEL_TEST(choice_generic_payload_interpolation_tostring_6465,
           issue_repro_hole_and_tostring_agree_for_both_payload_states_spec_35_7) {
    JitResult r = runAot(program(
        "    var pn: Col? := null;\n"
        "    var pv: Col? := Col.Rgb(7);\n"
        "    var n := W<Col>.Ok(pn);\n"
        "    var v := W<Col>.Ok(pv);\n"
        "    Console.WriteLine($\"{W<Col>.Ok(pn)}\");\n"
        "    Console.WriteLine(n.ToString());\n"
        "    Console.WriteLine($\"{W<Col>.Ok(pv)}\");\n"
        "    Console.WriteLine(v.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    std::string("W.Ok(v: null)\n"
                                "W.Ok(v: null)\n"
                                "W.Ok(v: Col.Rgb(v: 7))\n"
                                "W.Ok(v: Col.Rgb(v: 7))\n"));
}

// The same requirement stated as an EQUALITY between the two renderings rather
// than only as a literal block: rows [0]/[1] are the absent payload's hole and
// call, rows [2]/[3] the present payload's. Asserting the pairs equal AND that
// each pair is the spec text is what makes "the hole and the call agree" a
// named property of the row — a fix that kept both wrong in the same way, or
// that made only one of them right, fails here.
BEZEL_TEST(choice_generic_payload_interpolation_tostring_6465,
           hole_and_tostring_renderings_must_agree_spec_8_5) {
    JitResult r = runAot(program(
        "    var pn: Col? := null;\n"
        "    var pv: Col? := Col.Rgb(7);\n"
        "    var n := W<Col>.Ok(pn);\n"
        "    var v := W<Col>.Ok(pv);\n"
        "    Console.WriteLine($\"{n}\");\n"
        "    Console.WriteLine(n.ToString());\n"
        "    Console.WriteLine($\"{v}\");\n"
        "    Console.WriteLine(v.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    std::vector<std::string> lines = linesOf(r);
    BEZEL_EXPECT_EQ(lines.size(), static_cast<size_t>(4));
    if (lines.size() == 4) {
        BEZEL_EXPECT_EQ(lines[0], std::string("W.Ok(v: null)"));
        BEZEL_EXPECT_EQ(lines[1], std::string("W.Ok(v: null)"));
        BEZEL_EXPECT_EQ(lines[2], std::string("W.Ok(v: Col.Rgb(v: 7))"));
        BEZEL_EXPECT_EQ(lines[3], std::string("W.Ok(v: Col.Rgb(v: 7))"));
        // The disagreement this issue exists for, asserted directly.
        BEZEL_EXPECT_EQ(lines[0], lines[1]);
        BEZEL_EXPECT_EQ(lines[2], lines[3]);
        // And the hole is not vacuously agreeing by being empty on both legs.
        BEZEL_EXPECT_EQ(lines[0], std::string("W.Ok(v: null)"));
        BEZEL_EXPECT_EQ(lines[2], std::string("W.Ok(v: Col.Rgb(v: 7))"));
    }
}

// §8.5's component rule without the `?`: a payload spelled `T` at a generic
// choice argument. MEASURED, this spelling was ALSO empty through the hole
// before the fix — the payload type was the bare parameter, whatever its
// declared nullability — so this row is the one that shows the fix supplies an
// instantiation rather than a nullability flag. It is also the control against
// a fix that marked the substituted payload nullable unconditionally: the
// payload is present, so an unconditional `?` renders through the null-safe
// nullable-value arm and this text changes.
BEZEL_TEST(choice_generic_payload_interpolation_tostring_6465,
           non_nullable_generic_payload_hole_renders_the_value_spec_8_5) {
    JitResult r = runAot(program(
        "    var c := Col.Rgb(7);\n"
        "    var n := WN<Col>.Ok(c);\n"
        "    Console.WriteLine($\"{n}\");\n"
        "    Console.WriteLine(n.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    std::string("WN.Ok(v: Col.Rgb(v: 7))\n"
                                "WN.Ok(v: Col.Rgb(v: 7))\n"));
}

// §35.7's payload-free row is `"TypeName.CaseName"` with no parentheses and no
// component, so it needs no instantiation — it is the CONTROL that the fix did
// not change a path that was already correct, and the guard against a fix that
// disturbed the shared `switch` / PHI join the payload arm edits.
BEZEL_TEST(choice_generic_payload_interpolation_tostring_6465,
           payload_free_generic_choice_hole_is_unchanged_spec_35_7) {
    JitResult r = runAot(program(
        "    var n := WG<Col>.No;\n"
        "    Console.WriteLine($\"{n}\");\n"
        "    Console.WriteLine(n.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r), std::string("WG.No\nWG.No\n"));
}

// §8.5's component rule reached through a RECORD component rather than a
// direct choice receiver: `emitRecordToString` calls `emitValueToString` per
// component, and that path already passed the component's own type down. This
// row is green on both legs of the fix and is here to keep the hole-path change
// from being read as "the hole path was the only instTy-less one" — the record
// path was not, and the enumeration this commit reports says so.
BEZEL_TEST(choice_generic_payload_interpolation_tostring_6465,
           choice_reached_as_a_record_component_is_unchanged_spec_8_5) {
    JitResult r = runAot(program(
        "    var pn: Col? := null;\n"
        "    var rec := new R(W<Col>.Ok(pn));\n"
        "    Console.WriteLine($\"{rec}\");\n"
        "    Console.WriteLine(rec.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    std::string("R(N: W.Ok(v: null))\n"
                                "R(N: W.Ok(v: null))\n"));
}

// The same hole and call under the CHECKED oracle, so the emitted module is
// verified (`verifyModule`) and not only the native run. Passing an
// instantiation changes which arm of `emitChoiceToString`'s payload resolution
// runs, and that arm ORs a nullability flag onto a `sema::TypeRef` the
// renderer then dispatches on — an unchecked native run cannot see an
// IR-verify failure from it.
BEZEL_TEST(choice_generic_payload_interpolation_tostring_6465,
           checked_oracle_verifies_the_generated_module_for_both_payload_states_spec_35_7) {
    JitResult r = runAotChecked(program(
        "    var pn: Col? := null;\n"
        "    var pv: Col? := Col.Rgb(7);\n"
        "    var n := W<Col>.Ok(pn);\n"
        "    var v := W<Col>.Ok(pv);\n"
        "    Console.WriteLine($\"{n}\");\n"
        "    Console.WriteLine(n.ToString());\n"
        "    Console.WriteLine($\"{v}\");\n"
        "    Console.WriteLine(v.ToString());\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    std::string("W.Ok(v: null)\n"
                                "W.Ok(v: null)\n"
                                "W.Ok(v: Col.Rgb(v: 7))\n"
                                "W.Ok(v: Col.Rgb(v: 7))\n"));
}