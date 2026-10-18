// Codegen — #6169: a generic choice case payload spelled `T?` LOST the `?`
// when the payload type was resolved for §35.7's equality/hash fold, so a
// nullable choice at a choice argument was read back as the BARE choice.
//
// A payload spells its nullability at the PAYLOAD SITE (`Ok(v: T?)`); the
// concrete type ARGUMENT carries none of it (`W<Col>`'s `Col` is non-nullable).
// Both payload-type resolutions in `emitChoiceEquality` / `emitChoiceHashCode`
// — the `instTy->genericArgs` substitution and `resolveTypeParamRef` —
// returned the bare substituted type. The extracted values, however, ARE the
// tagged `{i8 hasValue; <choiceStruct>}` wrapper (`nullableValueStructTy`'s
// `isNullableChoiceTy` arm), so the mismatch skipped `emitValueEquality`'s
// null-safe nullable-value arm (`isNullableValueTy`, which DOES include
// `isNullableChoiceTy`) and fell into the bare-choice arm: it read the
// `hasValue` flag as the case tag and indexed payload slot `{2,0}` on a
// two-field wrapper. `CreateExtractValue`'s null result then segfaulted in
// `Value::setName` — bezelc exit 139, with no diagnostic.
//
// Per §9.2 ("`T?` where `T` is admissible — the comparison is the null-safe
// `=` of §8.4") and §8.4, `Col?` IS an admissible equality component, so
// sema accepting the program is CORRECT and the codegen read was the defect:
// this is a compile-crash, not an over-rejection to be tightened.
//
// All rows are compiled and RUN, so a fix that merely stopped the crash while
// comparing the hasValue flag as the tag (or treating `null` as equal to
// `Col.Red(0)`) would be red, not green.

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `Col` is a value-like auto-hashable choice (§9.2's admissible list); `W<T>`
// gives it a case payload spelled `T?` — the shape of the issue.
const char* const kPrelude = R"(module M;
choice Col Red, Rgb(v: int); end Col;
choice W<T> Ok(v: T?), No; end W;
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

}  // namespace

// §9.2 / §8.4 / §35.7 — the issue's literal repro. `=` on `W<Col>` must
// compile and run. Before the fix: bezelc SIGSEGV (exit 139, no diagnostic)
// inside `CreateExtractValue`'s null result, and the harness saw a compile
// crash rather than a run.
BEZEL_TEST(choice_nullable_generic_payload_equality_6169,
           issue_repro_equality_on_w_col_compiles_and_runs_spec_9_2_t_nullable) {
    JitResult r = runAot(program(
        "    var p1: Col? := Col.Rgb(1);\n"
        "    var p2: Col? := Col.Rgb(1);\n"
        "    var a := W<Col>.Ok(p1);\n"
        "    var b := W<Col>.Ok(p2);\n"
        "    if not (a = b) then return 0; end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §8.4 null-safe `=`: same case, one payload null and one present → NOT
// equal; and the reverse. A bare-choice read of the hasValue flag would make
// these agree. Before the fix: bezelc SIGSEGV.
BEZEL_TEST(choice_nullable_generic_payload_equality_6169,
           null_payload_compared_with_present_payload_is_unequal_spec_8_4) {
    JitResult r = runAot(program(
        "    var pv: Col? := Col.Rgb(1);\n"
        "    var pn: Col? := null;\n"
        "    var v := W<Col>.Ok(pv);\n"
        "    var n := W<Col>.Ok(pn);\n"
        "    if v = n then return 0; end;\n"
        "    if n = v then return 0; end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §8.4 null-safe `=`: two null payloads of the same case ARE equal (both
// null → equal), and a null payload differs from a DIFFERENT present case.
// This is the row that fails if the fix compares the hasValue flag as the
// case tag — `null` (hasValue 0) would equal `Red` (tag 0). Before the fix:
// bezelc SIGSEGV.
BEZEL_TEST(choice_nullable_generic_payload_equality_6169,
           two_null_payloads_of_one_case_are_equal_spec_8_4) {
    JitResult r = runAot(program(
        "    var pn: Col? := null;\n"
        "    var pr: Col? := Col.Red;\n"
        "    var n := W<Col>.Ok(pn);\n"
        "    var m := W<Col>.Ok(pn);\n"
        "    var red := W<Col>.Ok(pr);\n"
        "    if not (n = m) then return 0; end;\n"
        "    if n = red then return 0; end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §8.4 null-safe `=`: a payload-free case still compares by tag alone, and a
// payload-free case differs from the same case carrying a null payload.
BEZEL_TEST(choice_nullable_generic_payload_equality_6169,
           payload_free_case_still_compares_by_tag_alone_spec_35_7) {
    JitResult r = runAot(program(
        "    var pn: Col? := null;\n"
        "    var a := W<Col>.No;\n"
        "    var b := W<Col>.No;\n"
        "    var n := W<Col>.Ok(pn);\n"
        "    if not (a = b) then return 0; end;\n"
        "    if a = n then return 0; end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §35.7's hash is the SAME fold over the same payload list, and
// `emitChoiceHashCode` carried the same two payload-type resolutions — so
// `GetHashCode` on the same instantiation crashed too (measured: exit 139).
// Run through `runAotChecked` so the emitted IR is verified as well.
BEZEL_TEST(choice_nullable_generic_payload_equality_6169,
           get_hash_code_on_w_col_compiles_and_agrees_with_equality_spec_35_7) {
    JitResult r = runAotChecked(program(
        "    var p1: Col? := Col.Rgb(1);\n"
        "    var p2: Col? := Col.Rgb(1);\n"
        "    var pn: Col? := null;\n"
        "    var a := W<Col>.Ok(p1);\n"
        "    var b := W<Col>.Ok(p2);\n"
        "    var n := W<Col>.Ok(pn);\n"
        "    if a.GetHashCode() <> b.GetHashCode() then return 0; end;\n"
        "    if a.GetHashCode() = n.GetHashCode() then return 0; end;\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}