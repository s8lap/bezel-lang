// Codegen — #6458: a generic choice case payload spelled `T?` LOST the `?`
// when the payload type was resolved for §35.7's generated `ToString`, so a
// nullable choice at a choice argument was read back as the BARE choice.
//
// A payload spells its nullability at the PAYLOAD SITE (`choice W<T>
// Ok(v: T?)`); the concrete type ARGUMENT carries none of it (`W<Col>`'s `Col`
// is non-nullable). BOTH payload-type resolutions in `emitChoiceToString` —
// the `instTy->genericArgs` substitution (#4278) and `resolveTypeParamRef` —
// returned the bare substituted type. The extracted `fieldVal`, however, IS
// the tagged `{i8 hasValue; <choiceStruct>}` wrapper
// (`nullableValueStructTy`'s `isNullableChoiceTy` arm, #2447), so the
// mismatch skipped `emitValueToString`'s null-safe nullable-value arm
// (`isNullableValueTy`, which DOES include `isNullableChoiceTy`) and fell into
// the bare-choice arm: it read the `hasValue` flag as the case tag and indexed
// payload slot `{2,0}` on a two-field wrapper. `CreateExtractValue`'s null
// result then segfaulted in `llvm::Value::setName` — bezelc exit 139 with no
// diagnostic.
//
// Per §9.2 ("`T?` where `T` is admissible") and §35.7 (`TypeName.CaseName(
// field1: value1, …)`), `Col?` IS an admissible payload and the rendering is
// generated; sema accepting the program is CORRECT and the codegen read was the
// defect. This is a compile crash, not an over-rejection to be tightened.
//
// Every row is compiled AND run and asserts the exact rendered text, so a fix
// that merely stopped the crash — reading the `hasValue` flag as the case tag,
// or rendering the wrapper as the bare choice — is red, not green.

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `Col` is a value-like auto-hashable choice (§9.2's admissible list) with a
// payload-bearing first-later case, so the pre-fix `{2,0}` out-of-range
// extract is actually reached; `W<T>` gives it a case payload spelled `T?` —
// the shape of the issue. `W2` spells the SAME nullable payload concretely and
// is what exercises the `resolveTypeParamRef` else-arm (a non-generic receiver
// carries no `genericArgs`, so `instTy` is empty there) — the other of the two
// resolutions the fix touches. `WN<T>` spells the payload WITHOUT the `?`: the
// rendering control — a non-nullable `T` payload must still render as the bare
// value, which is what the OR (as opposed to an unconditional "mark the
// substituted payload nullable") must leave alone. It is NOT the control for
// that distinction: MEASURED, a planted "mark the payload nullable
// unconditionally" sentinel measures 7/7 GREEN here, because a bare `int`
// payload is not a struct, so `emitValueToString`'s null-safe nullable-value arm
// never fires for it and the OR is unobservable. The discriminating spelling is
// a STRUCT payload at the non-nullable site — `WN<Col>.Ok(Col.Rgb(7))` or
// `WN<Pt>.Ok(new Pt(7, 8))` — which is green when fixed and DID-NOT-RUN under
// that sentinel. `R` is the §8.5
// component-rule path: a record whose field is the generic choice, so
// `emitRecordToString` reaches the same `emitChoiceToString` payload resolution
// without a direct `.ToString()` call on the choice.
const char* const kPrelude = R"(module M;
choice Col Red, Rgb(v: int); end Col;
choice W<T> Ok(v: T?), No; end W;
choice W2 Ok(v: Col?), No; end W2;
choice WN<T> Ok(v: T), No; end WN;
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

}  // namespace

// §35.7: `TypeName.CaseName(field1: value1, …)`. The issue's literal repro —
// a null payload of a generic choice argument rendered through the generated
// ToString. Before the fix: bezelc SIGSEGV (exit 139, no diagnostic) inside
// `CreateExtractValue`'s null result, so the harness saw a compile crash
// rather than a run. The `null` in the field slot is what a null payload must
// render as (§8.5/§18.5); a fix that treated the `hasValue` flag as the case
// tag would render the bare `Col` head instead and fail this string compare.
BEZEL_TEST(choice_nullable_generic_payload_tostring_6458,
           issue_repro_tostring_on_w_col_ok_null_renders_the_null_payload_spec_35_7) {
    JitResult r = runAot(program(
        "    var pn: Col? := null;\n"
        "    var n := W<Col>.Ok(pn);\n"
        "    Console.WriteLine(n.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r), std::string("W.Ok(v: null)\n"));
}

// §35.7: the same instantiation with a PRESENT payload renders the inner
// choice by its own §35.7 format. This is the row that separates a correct
// fix from one that always renders `null` — the `hasValue` branch has to reach
// the inner choice's discriminant and payload, not just print the literal.
BEZEL_TEST(choice_nullable_generic_payload_tostring_6458,
           present_payload_renders_the_inner_choice_not_null_spec_35_7) {
    JitResult r = runAot(program(
        "    var pv: Col? := Col.Rgb(7);\n"
        "    var v := W<Col>.Ok(pv);\n"
        "    Console.WriteLine(v.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r), std::string("W.Ok(v: Col.Rgb(v: 7))\n"));
}

// Both spellings of the nullable payload in one program: the `T?` generic
// argument form and the concrete `Col?` form. The second one is NOT the same
// code path — with no `genericArgs` on the receiver, `emitChoiceToString` takes
// the `resolveTypeParamRef` else-arm, which dropped the `?` identically — and
// a fix that only OR-ed the nullability into the substitution branch would fix
// the first and leave this one crashing.
BEZEL_TEST(choice_nullable_generic_payload_tostring_6458,
           concrete_and_generic_nullable_payloads_both_render_spec_35_7) {
    JitResult r = runAot(program(
        "    var pn: Col? := null;\n"
        "    var pv: Col? := Col.Rgb(3);\n"
        "    var a := W2.Ok(pn);\n"
        "    var b := W2.Ok(pv);\n"
        "    var c := W<Col>.Ok(pn);\n"
        "    Console.WriteLine(a.ToString());\n"
        "    Console.WriteLine(b.ToString());\n"
        "    Console.WriteLine(c.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    std::string("W2.Ok(v: null)\n"
                                "W2.Ok(v: Col.Rgb(v: 3))\n"
                                "W.Ok(v: null)\n"));
}

// §35.7's payload-free row is `"TypeName.CaseName"` with no parentheses. It is
// NOT "unaffected" by this fix — it renders through the same `emitValueToString`
// / `emitChoiceToString` edge as the payload arm (this file's rows are 7/7 red
// pre-fix and 7/7 green post-fix) — so the name says what the row actually
// pins: the payload-free case of the SAME instantiation still renders its way.
// It shares the same `switch` and the same PHI join as the payload arm the fix
// edits, so it is what catches a fix that disturbed the control flow (a fix
// that emitted the hasValue-tag arm unconditionally would leak that arm's
// formatting into this case).
BEZEL_TEST(choice_nullable_generic_payload_tostring_6458,
           payload_free_case_of_the_same_instantiation_also_renders_spec_35_7) {
    JitResult r = runAot(program(
        "    Console.WriteLine(W<Col>.No.ToString());\n"
        "    Console.WriteLine(W2.No.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r), std::string("W.No\nW2.No\n"));
}

// §8.5's component rule reached through a RECORD component rather than a
// direct `.ToString()` call: `emitRecordToString` calls `emitValueToString`
// per component, which reaches the SAME `emitChoiceToString` payload
// resolution. This keeps the fix from being narrow to the direct-call
// lowering (`lowerValueSemMethodOn`).
//
// This row also reaches #6169's spelling — the equality/hash fold at
// `emitChoiceEquality` / `emitChoiceHashCode` — because a record's generated
// `=` thunk (`boxRecordEqualsThunkCore`) is emitted whether or not the program
// compares anything, so merely DECLARING `R` is enough. #6169 is in this tree
// (`eee7ffaa`), so both fixes are live together and the record renders
// `R(N: W.Ok(v: null))` with nothing hand-applied.
BEZEL_TEST(choice_nullable_generic_payload_tostring_6458,
           choice_reached_as_a_record_component_renders_spec_8_5) {
    JitResult r = runAot(program(
        "    var pn: Col? := null;\n"
        "    var n := W<Col>.Ok(pn);\n"
        "    var rec := new R(n);\n"
        "    Console.WriteLine(rec.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r), std::string("R(N: W.Ok(v: null))\n"));
}

// NOT the interpolation-hole spelling `$"{n}"` — that one is a DIFFERENT
// call site, and a defect in its own right: `LlvmBackend_Exprs.cpp`'s hole
// dispatch passed no `instTy` at all, so nothing was substituted (not even
// the `?`), and it rendered the payload slot EMPTY: `W.Ok(v: )` for BOTH a
// null and a present payload. §35.7 specifies `field1: value1`. That site
// was FIXED by #6465, which passes the hole's own instantiation; the
// rendering it asserts is now pinned by
// `choice_generic_payload_interpolation_tostring_6465_test.cpp`.

// §9.2: a NON-NULLABLE `T` payload is the rendering control for this fix. It
// must render exactly as it did before — the `pTy.nullable` OR added at both
// resolutions is a no-op when the payload site declares no `?`, and this row is
// what shows it. It is NOT a mutation sentinel for "mark the payload nullable
// unconditionally": MEASURED, a planted sentinel of that kind leaves this row
// GREEN, because a bare `int` is not a struct, so `emitValueToString`'s
// null-safe nullable-value arm never fires for it and the wrong flag is
// unobservable in the rendered text. The spelling that discriminates is a STRUCT
// payload at the same non-nullable site (`WN<Col>.Ok(Col.Rgb(7))`,
// `WN<Pt>.Ok(new Pt(7, 8))`): green when fixed, DID-NOT-RUN under the sentinel.
BEZEL_TEST(choice_nullable_generic_payload_tostring_6458,
           non_nullable_payload_control_is_unchanged_spec_9_2) {
    JitResult r = runAot(program(
        "    var v := WN<int>.Ok(7);\n"
        "    Console.WriteLine(v.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r), std::string("WN.Ok(v: 7)\n"));
}

// §9.2 / §8.3: the same instantiation run through the CHECKED oracle, so the
// emitted module is verified (`verifyModule`) and not only the native run —
// this fix moves control flow inside `emitChoiceToString` (the null-safe
// nullable-value dispatch), and an unchecked run cannot see an IR-verify
// failure on the generated wrapper. Scope note: this row deliberately does NOT
// also assert `=` / `GetHashCode` on `W<Col>` — those are the OTHER two
// spellings of this defect, fixed in `emitChoiceEquality` /
// `emitChoiceHashCode` by #6169, which IS in this tree (its own rows cover
// them); duplicating them here would make this file a second, weaker copy of
// #6169's contract rather than this commit's.
BEZEL_TEST(choice_nullable_generic_payload_tostring_6458,
           checked_oracle_verifies_the_generated_module_for_both_payload_states_spec_35_7) {
    JitResult r = runAotChecked(program(
        "    var pn: Col? := null;\n"
        "    var pv: Col? := Col.Rgb(7);\n"
        "    var n1 := W<Col>.Ok(pn);\n"
        "    var v  := W<Col>.Ok(pv);\n"
        "    Console.WriteLine(n1.ToString());\n"
        "    Console.WriteLine(v.ToString());\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    std::string("W.Ok(v: null)\n"
                                "W.Ok(v: Col.Rgb(v: 7))\n"));
}
