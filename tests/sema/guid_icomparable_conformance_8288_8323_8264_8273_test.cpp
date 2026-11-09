// Regression tests for issues #8288, #8323, #8264 and #8273 — the built-in
// `Guid` and the `IComparable<Guid>` the STANDARD LIBRARY declares for it.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §2.6 (v0.355) declares the built-in value record
//       public record Guid: IComparable<Guid>, IEquatable<Guid>
//         // CompareTo = lexicographic over the 16 canonical big-endian bytes
//         public method CompareTo(other: Guid): int;
//         …
//       end Guid;
//     ⇒ the built-in `Guid` CONFORMS to `IComparable<Guid>`, exactly as
//     stdlib §2.5's temporal records (`DateTime`, `TimeSpan`, `DateOnly`,
//     `TimeOnly`) conform to `IComparable<Self>`.
//
//   * stdlib §2.8 (v0.355) declares the constraint's authority
//       public interface IComparable<T>
//         method CompareTo(other: T): int;
//       end IComparable;
//
//   * stdlib §5.1 (v0.355) declares the no-selector `Min()` / `Max()` as
//       public extension IEnumerable<T> where T: IComparable<T>
//         public method Min(): T;
//         public method Max(): T;
//       end;
//     ⇒ the no-selector form exists ONLY where the element type satisfies
//     `T: IComparable<T>`.
//
//   * stdlib §4.4 (v0.355) declares `Array<T>.Sort()` under the SAME
//     constraint (`where T: IComparable<T>`), and stdlib §1's "Conditional
//     members" row records it.
//
//   * spec §14.2 (v0.355) *Interface conformance* — "For an interface I and a
//     candidate type T, T conforms to I exactly when one of the following
//     holds": rule 2 admits "records and enums implementing it", so the
//     built-in value record `Guid` conforms at `IComparable<Guid>`.
//
// THIS FILE'S SCOPE — WHAT IS FIXED AND WHAT IS NOT.
//
//   * #8273 is FIXED: the `Sort()` refusal's DIAGNOSTIC TEXT had two defects.
//     It spelled the nullable element with a PREFIXED marker (`?Guid`), which
//     is NOT valid Bezel — the grammar spells a nullable type with a TRAILING
//     `?` — so the suggested remedy could not be pasted back into a program;
//     and it cited the retired spec revision `v0.276` where the current one is
//     `v0.355`. Rows 1-4 pin the corrected text and its PASTEABILITY.
//
//   * #8323 / #8264 / #8288 are NOT closable in this lane and are NOT claimed
//     here. Their remaining half is in `src/codegen/LlvmBackend_Calls.cpp`
//     (an OWNED file) — the collection `Min()` / `Max()` lowering has no
//     `Guid` arm (its own comment says so: "`Guid` … is NOT here: it is a
//     `{i64,i64}` STRUCT whose §2.6 order is 'lexicographic over the 16
//     canonical big-endian bytes', not a scalar-word order, so it needs a
//     structural comparison of its own"), and the §5.1 "a type to an
//     interface it implements or conforms to … records … by declaration,
//     boxing" conversion of a built-in value record into `IComparable<Guid>`
//     needs a boxing itable that does not exist (the primitive path's
//     `buildPrimBoxItmap` covers PRIMITIVES only). Row 8 pins the property
//     that DOES hold for every one of those shapes today: the program is
//     REFUSED BY A LOCATED DIAGNOSTIC — never a malformed-IR abort, never a
//     silent wrong answer.
//
// ORACLE. `runAot` (the AOT path — the strict oracle). Every REFUSING row
// asserts the SPECIFIC diagnostic text and that the refusal is a COMPILE-TIME
// refusal at a REAL location. Every ACCEPTING row asserts the EXACT STDOUT —
// a VALUE oracle, never "no diagnostic was produced".
//
// THE CONTROLS are the positive twins: a numeric element still prints its
// minimum; a `string` element still prints its minimum; a user record
// declaring `IComparable<Rec>` with its own `CompareTo` still prints its
// minimum; the SELECTOR overload over a `Guid` element still prints the right
// value; `Math.Min` / `Math.Max` / `Math.Clamp` over two `Guid`s still print
// the right value (stdlib §2.2's `where T: IComparable<T>` — the `Guid` arm
// of that lowering is already in the tree); `Array<Guid>.Sorted()` still
// prints the right value (stdlib §4.4 — #8237); and `Guid.CompareTo` itself
// still orders correctly (stdlib §2.6).

#include <regex>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out.
std::string out(const std::string& program) {
    JitResult r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// fix #8273: the CORRECTED `Sort()` refusal text. Two properties are pinned
// at once, because the issue is about both:
//
//   * the nullable element is spelled with the TRAILING marker (`Guid?`), the
//     form the GRAMMAR accepts — the pre-fix text spelled it `?Guid`, which
//     does not parse;
//   * the message cites the CURRENT spec revision (`v0.355`), not the retired
//     `v0.276`.
const char* kSortDiagGuid =
    "Array<Guid?>.Sort() requires T: IComparable<T> "
    "(spec §14.2, stdlib §4.4, v0.355); Guid? does not implement "
    "IComparable<Guid?>. Use Sort(comparer: (Guid?, Guid?) -> int) instead.";

// The same message with the `Object` element — the issue records that "the
// same text with `Object` shows the stale `v0.276`", so the control carries
// the same two text fixes.
const char* kSortDiagObject =
    "Array<Object?>.Sort() requires T: IComparable<T> "
    "(spec §14.2, stdlib §4.4, v0.355); Object? does not implement "
    "IComparable<Object?>. Use Sort(comparer: (Object?, Object?) -> int) "
    "instead.";

// The retired spellings — the pre-fix text. A row that pins the NEW text
// must not also match the OLD one.
const char* kSortDiagOldVersion = "v0.276";
const char* kSortDiagOldMarker = "?Guid";

// The line the diagnostic carrying `sub` is reported at (0 when there is
// none). `Driver.cpp` prints each diagnostic as `<path>:<line>:<col>:
// <message>`, and that TEXT is what the AOT harness packs into one
// Diagnostic — so the real location is read back off the message.
int errorLineOf(const std::string& program, const std::string& sub) {
    JitResult r = runAot(program);
    static const std::regex kLoc(R"(:(\d+):(\d+): )");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::size_t pos = d.message.find(sub);
        if (pos == std::string::npos) continue;
        std::size_t lineStart = d.message.rfind('\n', pos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        std::size_t lineEnd = d.message.find('\n', pos);
        std::string line = d.message.substr(
            lineStart, (lineEnd == std::string::npos ? d.message.size()
                                                      : lineEnd) -
                           lineStart);
        std::smatch m;
        if (!std::regex_search(line, m, kLoc)) return 0;
        return std::stoi(m[1].str());
    }
    return 0;
}

// The issue's own repro for #8273: a NULLABLE `Guid` element, sorted without
// a comparer. Line 6 carries the `a.Sort()` call.
const char* kGuidNullableSort =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var a := new Array<Guid?>();\n"
    "    a.Add(Guid.Empty);\n"
    "    a.Sort();\n"
    "    Console.WriteLine(a.Count);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same shape with the `Object` element (the issue's control).
const char* kObjectNullableSort =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var a := new Array<Object?>();\n"
    "    a.Add(null);\n"
    "    a.Sort();\n"
    "    Console.WriteLine(a.Count);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The no-selector `Min()` / `Max()` over a `Guid` element. stdlib §5.1
// admits it (the element conforms to `IComparable<Guid>`); the refusal that
// remains is the CODEGEN half — the collection lowering has no `Guid` arm.
const char* kGuidMin =
    "module M;\n"
    "class C public static method Main(): int\n"
    " var a := new Array<Guid>();\n"
    " a.Add(Guid.Parse(\"22222222-2222-2222-2222-222222222222\"));\n"
    " a.Add(Guid.Parse(\"00000001-0000-0000-0000-000000000000\"));\n"
    " Console.WriteLine(a.Min());\n"
    " return 0; end Main; end C;\n";

}  // namespace

// ---------------------------------------------------------------------------
// ROW 1 — #8273: the `Sort()` refusal's TEXT. The nullable element is
// spelled `Guid?` (the form the grammar accepts) and the message cites the
// CURRENT spec revision `v0.355`.
//
// Pre-fix: "Array<?Guid>.Sort() requires T: IComparable<T> (spec §14.2,
// stdlib §4.4, v0.276); ?Guid does not implement IComparable<?Guid>. Use
// Sort(comparer: (?Guid, ?Guid) -> int) instead."
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           sort_refusal_spells_the_nullable_element_with_a_trailing_marker) {
    JitResult r = runAot(kGuidNullableSort);
    BEZEL_EXPECT_FALSE(r.ran);
    // The WHOLE corrected message, verbatim.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSortDiagGuid));
    // NOT the retired spellings.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSortDiagOldVersion));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSortDiagOldMarker));
    // A compile-time refusal at a REAL location (not a crash / hang).
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // AT THE CALL: line 6 carries `a.Sort()`.
    BEZEL_EXPECT_EQ(errorLineOf(kGuidNullableSort, "requires T: IComparable<T>"),
                    6);
}

// ---------------------------------------------------------------------------
// ROW 2 — #8273 CONTROL: the same message over an `Object` element carries
// the same two text fixes (the issue records that "the same text with
// `Object` shows the stale `v0.276`").
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           sort_refusal_over_an_object_element_carries_the_same_two_fixes) {
    JitResult r = runAot(kObjectNullableSort);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSortDiagObject));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSortDiagOldVersion));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(errorLineOf(kObjectNullableSort, "requires T: IComparable<T>"),
                    6);
}

// ---------------------------------------------------------------------------
// ROW 3 — #8273: the SUGGESTED REMEDY IS PASTEABLE. The message names the
// comparer type `(Guid?, Guid?) -> int`; a program must be able to spell that
// TYPE and pass such a comparer to `Sort`. Pre-fix the message named
// `(?Guid, ?Guid) -> int`, which does not even PARSE.
//
// This is the row that proves the text fix is a fix and not a cosmetic edit:
// a remedy that cannot be pasted is not a remedy.
//
// ⚠ The comparer is passed POSITIONALLY. The diagnostic's remedy is a
// SIGNATURE (`Sort(comparer: (Guid?, Guid?) -> int)` — `comparer` is the
// stdlib §4.4 parameter name), and a NAMED argument is refused on this
// built-in member by a separate rule (#3398: "named arguments are not
// supported on this built-in member — the compiler carries no parameter
// names for it"). What #8273 is about is the TYPE the remedy names, and
// that is what this row pins.
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           the_suggested_remedy_type_is_pasteable) {
    BEZEL_EXPECT_EQ(
        out("module Conf;\n"
            "public class Program\n"
            "  public static method Main(): int\n"
            "    var a := new Array<Guid?>();\n"
            "    a.Add(Guid.Empty);\n"
            "    var cmp: (Guid?, Guid?) -> int := "
            "lambda(x: Guid?, y: Guid?) => 0;\n"
            "    a.Sort(cmp);\n"
            "    Console.WriteLine(a.Count);\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        "1\n");
}

// ---------------------------------------------------------------------------
// ROW 4 — #8273 CONTROL: the RETIRED spelling `(?Guid, ?Guid) -> int` is not
// valid Bezel — it does not parse. This row is the reason the trailing form
// is the right one: the pre-fix remedy named a type no program can spell.
//
// (A parse failure is a compile-time refusal with a real location, so the
// same location predicate applies.)
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           the_prefixed_nullable_marker_is_not_valid_bezel) {
    const std::string program =
        "module Conf;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var a := new Array<Guid?>();\n"
        "    a.Add(Guid.Empty);\n"
        "    var cmp: (?Guid, ?Guid) -> int := "
        "lambda(x: Guid?, y: Guid?) => 0;\n"
        "    a.Sort(cmp);\n"
        "    Console.WriteLine(a.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    // The PARSER rejected the prefixed marker.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "expected type name"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// ROW 5 — #8273 CONTROL: the NULLABLE `Guid` element stays REFUSED. The
// nullable question is NOT this lane's (#8241 is an open owner-ruling: does
// `T?` satisfy `IComparable<T?>`?). What this row pins is that the
// #8273 TEXT fix did not quietly ADMIT the nullable element — the diagnostic
// still exists, so the text fix is a fix and not a behaviour change.
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           the_nullable_guid_element_is_still_refused) {
    JitResult r = runAot(kGuidNullableSort);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "requires T: IComparable<T>"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "does not implement IComparable<Guid?>"));
    // NOT the defect's own signature: the malformed IR is gone.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Both operands to ICmp"));
}

// ---------------------------------------------------------------------------
// ROW 6 — #8264 CONTROL: `Math.Min` / `Math.Max` / `Math.Clamp` over two
// `Guid`s (stdlib §2.2's `where T: IComparable<T>`) — ALREADY correct in the
// tree (the `Guid` arm of that lowering dispatches the runtime's
// `bz_guid_compare`, which IS §2.6's "lexicographic over the 16 canonical
// big-endian bytes"). The row pins that it STAYS correct.
//
// stdlib §2.6's order is over the CANONICAL bytes, so the lesser of
// `2222…` and `1111…` is `1111…`.
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           math_min_max_clamp_over_two_guids_order_by_the_canonical_bytes) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    var a: Array<Guid> := "
            "[Guid.Parse(\"22222222-2222-2222-2222-222222222222\"), "
            "Guid.Parse(\"11111111-1111-1111-1111-111111111111\")];\n"
            "    Console.WriteLine(Math.Min(a[0], a[1]));\n"
            "    Console.WriteLine(Math.Max(a[0], a[1]));\n"
            "    Console.WriteLine(Math.Clamp(a[0], a[1], a[0]));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "11111111-1111-1111-1111-111111111111\n"
        "22222222-2222-2222-2222-222222222222\n"
        "22222222-2222-2222-2222-222222222222\n");
}

// ---------------------------------------------------------------------------
// ROW 7 — #8323 CONTROL: the SELECTOR overload over a `Guid` element
// (stdlib §5.1's `Min<U>(selector: (T) -> U): U` — the constraint is on `U`,
// NOT on the element type `T`) is ADMITTED and prints the lesser `Guid`.
// The selector form must not be broken by any gate change.
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           control_the_selector_form_over_a_guid_element_still_prints_the_lesser) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "class C public static method Main(): int\n"
            " var a := new Array<Guid>();\n"
            " a.Add(Guid.Parse(\"22222222-2222-2222-2222-222222222222\"));\n"
            " a.Add(Guid.Parse(\"00000001-0000-0000-0000-000000000000\"));\n"
            " Console.WriteLine(a.Min(lambda(g) => g));\n"
            " return 0; end Main; end C;\n"),
        "00000001-0000-0000-0000-000000000000\n");
}

// ---------------------------------------------------------------------------
// ROW 8 — #8323 / #8264: the no-selector `Min()` over a `Guid` element.
//
// stdlib §5.1 ADMITS it (the element conforms to `IComparable<Guid>`), but
// the ORDERING is the codegen half and it is NOT in this lane's hot file:
// the collection `Min()` / `Max()` lowering in `src/codegen/
// LlvmBackend_Calls.cpp` has no `Guid` arm (its own comment: "`Guid` … is
// NOT here: it is a `{i64,i64}` STRUCT whose §2.6 order is 'lexicographic
// over the 16 canonical big-endian bytes', not a scalar-word order, so it
// needs a structural comparison of its own"). The `Math` arm HAS one
// (row 6), the collection arm does not.
//
// What this row pins is the property that holds for EVERY state of that
// shape — today and after the codegen arm lands:
//
//   * the program is REFUSED BY A LOCATED DIAGNOSTIC naming the constraint
//     and the element (never a malformed-IR abort, never a silent wrong
//     answer);
//   * and the selector form (row 7) is the WORKING remedy.
//
// The row accepts EITHER end of the pending transition — the located refusal
// today, the printed minimum once the codegen arm lands — so it does not
// have to be rewritten when that lands. It refuses only the two shapes that
// are wrong at every point of the transition: a malformed-IR abort and a
// silent wrong answer.
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           no_selector_min_over_a_guid_element_is_refused_by_a_located_diagnostic) {
    JitResult r = runAot(kGuidMin);
    if (r.ran) {
        // The codegen arm has landed: the least `Guid` is printed.
        BEZEL_EXPECT_EQ(r.stdoutText,
                        "00000001-0000-0000-0000-000000000000\n");
        return;
    }
    // Still refused — by a LOCATED diagnostic that names the constraint and
    // the element type.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "IComparable<T>"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "Guid"));
    // NOT a malformed-IR abort and NOT a silent wrong answer.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Both operands to ICmp"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

// ---------------------------------------------------------------------------
// ROW 9 — #8323 CONTROL: a USER RECORD that declares `IComparable<Rec>` with
// its own `CompareTo` still sorts and still prints its minimum (stdlib §5.1,
// spec §14.2). A gate change that widened or narrowed the §5.1 constraint
// would break this row.
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           control_a_user_record_declaring_icomparable_still_prints_its_minimum) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "public record Rec(v: int): IComparable<Rec>\n"
            "  public method CompareTo(other: Rec): int => v - other.v;\n"
            "end Rec;\n"
            "class C public static method Main(): int\n"
            " var a := new Array<Rec>();\n"
            " a.Add(new Rec(5));\n"
            " a.Add(new Rec(2));\n"
            " Console.WriteLine(a.Min().v);\n"
            " return 0; end Main; end C;\n"),
        "2\n");
}

// ---------------------------------------------------------------------------
// ROW 10 — #8323 CONTROL: a NUMERIC element and a `string` element still
// print their minima (stdlib §9.1 / §9.6 — the primitives and `string`
// conform to `IComparable<T>` at their own type).
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           control_numeric_and_string_elements_still_print_their_minima) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "class C public static method Main(): int\n"
            " var i := new Array<int>();\n"
            " i.Add(3); i.Add(1);\n"
            " Console.WriteLine(i.Min());\n"
            " var s := new Array<string>();\n"
            " s.Add(\"b\"); s.Add(\"a\");\n"
            " Console.WriteLine(s.Min());\n"
            " return 0; end Main; end C;\n"),
        "1\na\n");
}

// ---------------------------------------------------------------------------
// ROW 11 — #8288 CONTROL: `IComparable<int>` and `IComparable<string>` — the
// §8.6 / §5.1 boxing of a PRIMITIVE into the builtin interface it conforms
// to (stdlib §9.1 / §9.6) — are ADMITTED and order correctly. This is the
// shape the built-in value record's own conversion would have to match.
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           control_primitive_boxes_into_icomparable_at_its_own_type) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "class C public static method Main(): int\n"
            " var a: IComparable<int> := 5;\n"
            " Console.WriteLine(a.CompareTo(6));\n"
            " var b: IComparable<string> := \"x\";\n"
            " Console.WriteLine(b.CompareTo(\"y\"));\n"
            " return 0; end Main; end C;\n"),
        "-1\n-1\n");
}

// ---------------------------------------------------------------------------
// ROW 12 — #8264 / #8273 CONTROL: `Guid.CompareTo` itself (stdlib §2.6) and
// `Array<Guid>.Sorted()` (stdlib §4.4 — #8237) still order by the canonical
// bytes. They are the two `Guid` orderings the tree ALREADY has, and this
// row pins that neither of them regressed.
// ---------------------------------------------------------------------------
BEZEL_TEST(guid_icomparable_conformance_8288_8323_8264_8273,
           control_guid_compare_to_and_array_guid_sorted_still_order_by_the_canonical_bytes) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "class C public static method Main(): int\n"
            " var g := Guid.Empty;\n"
            " Console.WriteLine(g.CompareTo("
            "Guid.Parse(\"11111111-1111-1111-1111-111111111111\")));\n"
            " var a: Array<Guid> := "
            "[Guid.Parse(\"22222222-2222-2222-2222-222222222222\"), "
            "Guid.Parse(\"11111111-1111-1111-1111-111111111111\")];\n"
            " var b := a.Sorted();\n"
            " Console.WriteLine(b[0]);\n"
            " return 0; end Main; end C;\n"),
        "-1\n11111111-1111-1111-1111-111111111111\n");
}
