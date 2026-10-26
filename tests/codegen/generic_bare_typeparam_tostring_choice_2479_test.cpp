// Codegen/sema — issue #2479: `val.ToString()` on a bare-type-parameter `T`
// substituted with a CHOICE had no codegen at all — a crash-preventing
// diagnostic (added alongside #2476) stood in for it. This suite implements
// language spec §35.7's format for the auto-hashable (value-like,
// admissible-payload) classification: "the generated ToString produces
// 'TypeName.CaseName(field1: value1, …)', and 'TypeName.CaseName' for a
// payload-free case." That format is unambiguous, so it is implemented for
// real (`Emitter::emitChoiceToString`, LlvmBackend_String.cpp) and wired
// into every place a choice's ToString can be reached:
//
//   1. A direct/concrete `.ToString()` call — `lowerValueSemMethodOn`'s
//      Choice branch (LlvmBackend_Calls2.cpp), the same dispatcher records
//      and enums already use.
//   2. A bare-T generic call under `where T: value` (this issue's own
//      literal scope) — the SAME dispatcher, reached through the
//      genericSubstSemaTy_ redirect that already exists for Record/Enum.
//   3. A choice-typed interpolation hole (`$"{c}"`) — interpolation
//      (LlvmBackend_Exprs.cpp) has its OWN hand-rolled ToString dispatch,
//      entirely separate from `lowerValueSemMethodOn`, and had no Choice
//      case at all: `$"{c}"` silently rendered just the bare type name
//      (indistinguishable from an ineligible-record default), dropping the
//      case/payload information — a DISTINCT gap found while regression-
//      testing this issue, not covered by its own original framing.
//   4. A choice-typed FIELD of an outer record/choice, formatted via
//      `emitValueToString` (LlvmBackend_String.cpp) — also had no Choice
//      branch; a nested choice payload rendered as an empty string.
//
// For the classification §35.7 does NOT define a format for — borrow-
// bearing, owning, or a value-like choice with an inadmissible payload —
// §35.7's own text pointedly omits ToString from its use-site-rejection
// list (unlike `=`/`<>`/`Equals`/`GetHashCode`), so no default can be
// inferred without guessing (CLAUDE.md rule 2: do not invent spec/stdlib
// behavior). The pre-existing #2476 diagnostic is KEPT for that narrower,
// still-undecided case — see the `non_auto_equality_bearing_*` tests below —
// rather than closing #2479 outright.
//
// A companion leak fix rides along (found while regression-testing, not
// pre-existing #2065 fallout — choice ToString had no codegen at all
// before this commit, so it could not have leaked before): `isPlusOneStringProducer`
// (LlvmBackend_Setup.cpp) recognized a concrete/generic RECORD `.ToString()`
// call as minting a fresh rc=1 heap string (so a `var s := x.ToString();`
// local's scope-end release drops it), but had no Choice case — every
// choice `.ToString()` call leaked its result (LSan-confirmed: 54-104
// bytes per call in this suite's own development) until the two mirrored
// Choice branches were added.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must
// be distinguished by asserting on the diagnostic TEXT, never by exit code
// or `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

const char* kColor =
    "choice Color Red, Green(value: int), Blue; end Color; ";
const char* kPair =
    "choice Pair P(x: int, y: string); end Pair; ";
const char* kNonAutoEqBearing =
    "class Widget end Widget; "
    "choice Holder Empty, Filled(item: Widget); end Holder; ";
const char* kNonAeqDiag =
    "member `ToString` on a choice that is not auto-hashable";

}  // namespace

// ── Direct/concrete receiver — the §35.7 format itself ──────────────────

// A payload-free case: "TypeName.CaseName" (§35.7).
BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           direct_payload_free_case_format) {
    const std::string program =
        std::string("module M; ") + kColor +
        "class C public static method Main(): int "
        "  var s := Color.Red.ToString(); "
        "  return (if s = \"Color.Red\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// A payload-bearing case with a mix of an int and a string field:
// "TypeName.CaseName(field1: value1, field2: value2)" (§35.7). The string
// field renders unquoted (identity, §9.6 — matching the record precedent).
BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           direct_payload_bearing_case_format) {
    const std::string program =
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  var s := Pair.P(3, \"hi\").ToString(); "
        "  return (if s = \"Pair.P(x: 3, y: hi)\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// Distinct cases of the same choice render distinctly, proving the active
// discriminant actually drives formatting (not a fixed/first-case default).
BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           distinct_cases_render_distinctly) {
    const std::string program =
        std::string("module M; ") + kColor +
        "class C public static method Main(): int "
        "  var r := Color.Red.ToString(); "
        "  var g := Color.Green(9).ToString(); "
        "  var b := Color.Blue.ToString(); "
        "  return (if r = \"Color.Red\" and g = \"Color.Green(value: 9)\" "
        "             and b = \"Color.Blue\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── The bare-T generic call — this issue's own literal repro shape ──────

BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           bare_t_generic_payload_free_value_correct) {
    const std::string program =
        std::string("module M; ") + kColor +
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(Color.Red); "
        "    return (if s = \"Color.Red\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           bare_t_generic_payload_bearing_value_correct) {
    const std::string program =
        std::string("module M; ") + kPair +
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(Pair.P(7, \"yo\")); "
        "    return (if s = \"Pair.P(x: 7, y: yo)\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Interpolation — a DISTINCT gap from this issue's own bare-T framing ──

// `$"{c}"` on a concrete (non-generic) choice-typed hole: before this fix,
// interpolation's own separate hand-rolled ToString dispatch
// (LlvmBackend_Exprs.cpp) had no Choice case and silently rendered just the
// bare type name ("Color"), dropping the case entirely — confirmed during
// this fix's own development, distinct from and not exercised by the
// #2476-added diagnostic (which only ever fired for the bare-T CALL form).
BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           interpolation_payload_bearing_value_correct) {
    const std::string program =
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  var p := Pair.P(3, \"hi\"); "
        "  var s := $\"got {p}\"; "
        "  return (if s = \"got Pair.P(x: 3, y: hi)\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Nested field — a choice-typed record field formats through the same
//    emitValueToString path emitRecordToString already uses for records ──

BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           nested_choice_field_of_record_value_correct) {
    const std::string program =
        std::string("module M; ") + kPair +
        "record Wrapper(tag: string, c: Pair); "
        "class C public static method Main(): int "
        "  var w := new Wrapper(\"t\", Pair.P(9, \"yo\")); "
        "  var s := w.ToString(); "
        "  return (if s = \"Wrapper(tag: t, c: Pair.P(x: 9, y: yo))\" "
        "             then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── The undecided classification: §35.7 defines no default, so the
//    pre-existing diagnostic is KEPT (not a crash, not an invented default) ──

// Direct call on a non-auto-hashable (borrow-bearing) choice.
BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           non_auto_equality_bearing_direct_call_diagnosed_not_crashed) {
    const std::string program =
        std::string("module M; ") + kNonAutoEqBearing +
        "class C public static method Main(): int "
        "  using var w := new Widget(); "
        "  var h := Holder.Filled(w); "
        "  var s := h.ToString(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, kNonAeqDiag));
    BEZEL_EXPECT_TRUE(hasError(program, "#2479"));
}

// Interpolation on a non-auto-hashable choice — the SAME undecided
// classification, reached through the separate interpolation dispatch path.
BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           non_auto_equality_bearing_interpolation_diagnosed_not_crashed) {
    const std::string program =
        std::string("module M; ") + kNonAutoEqBearing +
        "class C public static method Main(): int "
        "  using var w := new Widget(); "
        "  var h := Holder.Filled(w); "
        "  var s := $\"h={h}\"; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, kNonAeqDiag));
    BEZEL_EXPECT_TRUE(hasError(program, "#2479"));
}

// Bare-T generic call on a non-auto-hashable choice substitution.
BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           non_auto_equality_bearing_bare_t_diagnosed_not_crashed) {
    const std::string program =
        std::string("module M; ") + kNonAutoEqBearing +
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    using var w := new Widget(); "
        "    var s := F(Holder.Filled(w)); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(program, kNonAeqDiag));
    BEZEL_EXPECT_TRUE(hasError(program, "#2479"));
}

// ── Regression guard: record/enum/primitive bare-T ToString unaffected ──

BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           regression_record_bare_t_unaffected) {
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(new Pt(1, 2)); "
        "    return (if s = \"Pt(X: 1, Y: 2)\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Leak-freedom (mirrors #2475's own loop/runAotChecked idiom) ─────────

// A companion leak was found while implementing this fix: `emitChoiceToString`
// mints a fresh rc=1 heap string per call (the same shape `emitRecordToString`
// already does), but `isPlusOneStringProducer` (LlvmBackend_Setup.cpp) had no
// Choice recognizer — so `var s := c.ToString();` was treated as a BORROW
// (retained, never dropping the producer's own +1), leaking every call.
// LSan-confirmed during development: 54 bytes/call for a payload-bearing
// int-only case, 104 bytes/call with a string payload field. Looping (40
// iterations, the #2475/#1988 idiom) makes LSan's reachability scan reliable
// (a one-shot bound-then-read string can otherwise linger in a register and
// evade it).
namespace {
std::string choiceLoopCount(const char* preamble, const char* ctorExpr,
                            bool throughGenericT) {
    std::string body = throughGenericT
        ? "var s := F(" + std::string(ctorExpr) + "); "
        : "var s := (" + std::string(ctorExpr) + ").ToString(); ";
    std::string fMethod = throughGenericT
        ? "  public static method F<T>(val: T): string where T: value "
          "    return val.ToString(); "
          "  end F; "
        : "";
    return std::string("module M; ") + preamble +
           "class C " + fMethod +
           "  public static method Main(): int "
           "    var count := 0; "
           "    var i := 0; "
           "    while i < 40 do "
           "      " + body +
           "      if s.Length > 0 then count := count + 1; end; "
           "      i := i + 1; "
           "    end; "
           "    return count; "
           "  end Main; "
           "end C;";
}
}  // namespace

BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           direct_payload_bearing_lsan_clean) {
    JitResult r = runAotChecked(choiceLoopCount(
        "choice Item I(x: int, y: int); end Item; ", "Item.I(i, i + 1)",
        /*throughGenericT=*/false));
    BEZEL_EXPECT_TRUE(r.ran);         // compiled + LSan-clean
    BEZEL_EXPECT_EQ(r.exitCode, 40);  // every render byte-correct (Length > 0)
}

BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           direct_payload_free_lsan_clean) {
    JitResult r = runAotChecked(choiceLoopCount(
        "choice Color Red, Green(value: int), Blue; end Color; ",
        "Color.Red", /*throughGenericT=*/false));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// A STRING payload field — a materially different shape for refcounting
// (emitChoiceToString borrows the field's own string slot rather than
// releasing it, mirroring emitRecordToString's `fieldStrIsBorrow` rule) —
// confirmed the leak fix does not disturb that internal borrow/release
// discipline; only the OVERALL result's producer classification changes.
BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           direct_string_payload_field_lsan_clean) {
    JitResult r = runAotChecked(choiceLoopCount(
        "choice Named N(tag: string, n: int); end Named; ",
        "Named.N(\"item\", i)", /*throughGenericT=*/false));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

BEZEL_TEST(generic_bare_typeparam_tostring_choice_2479,
           bare_t_generic_payload_bearing_lsan_clean) {
    JitResult r = runAotChecked(choiceLoopCount(
        "choice Item I(x: int, y: int); end Item; ", "Item.I(i, i + 1)",
        /*throughGenericT=*/true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}
