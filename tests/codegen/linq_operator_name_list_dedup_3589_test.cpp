// Sema — issue #3589 (+ #3444): ten hand-spelled name lists answered "is this
// a stdlib §5/§5.1 LINQ operator name" across `ExprTypecheck_Calls.cpp`, each
// drifted independently. This fix:
//
//   1. Collapses the string-receiver `isLinqOp` (`.` form) and `isStringLinqOp`
//      (`?.` form) — a #3589 measurement found byte-identical, 38-name
//      copies — onto one table, `common::isStringLinqOperatorMemberName`
//      (`src/common/BuiltinRegistry.{hpp,cpp}`).
//   2. Collapses the four `isLinqOperator` copies (Array/OwningArray;
//      Stack/OwningStack/Queue/OwningQueue; Dictionary/OwningDictionary/
//      SortedDictionary; Set/OwningSet/SortedSet/LinkedList/OwningLinkedList)
//      onto one table, `common::isBuiltinCollectionLinqOperatorName` — the
//      union of the four (three were already byte-identical; the fourth, the
//      Array/OwningArray copy, was missing `ToArray` only because that
//      family's own `isDeclaredMethod` already admits it, so the union
//      changes no observable accept/reject decision there).
//   3. Removes five names — `ToList`, `Append`, `SequenceEqual`, `Join`,
//      `AsReadOnly` — from the `allowedOwningMethod`/`allowedNonOwningMethod`
//      44-item call-validity lists (~5208/5340). None is a real member of
//      `Array`/`OwningArray`/`Stack`/`Queue`/`Set`/`Dictionary`/`LinkedList`/
//      `SortedDictionary`/`SortedSet` per `docs/Bezel_Standard_Library.md`
//      §4/§5 — each name IS real, but on a DIFFERENT type
//      (`StringBuilder.Append`, `ArraySpan<T>.SequenceEqual`/`.AsReadOnly`,
//      `TaskScope.Join`/`string.Join`, and `ToList` does not exist anywhere
//      — stdlib §5's own text: "There is no `ToList()`..."). Confirmed by
//      grep: no codegen dispatch arm anywhere fires for any of the five on
//      these nine receiver families, so admitting the name here only routed
//      an already-doomed call to a WORSE, generic "#2457 no dispatch arm"
//      diagnostic instead of this predicate's own precise
//      "no member ... (§22.5 / §4.x)" one. This test pins that diagnostic
//      change (FAIL-WITHOUT: `#2457`'s "produced no value, and no dispatch
//      arm..." wording; PASS-WITH: the collection-specific "no member"
//      wording).
//
// `OfType`, `Reversed`, and `Sorted` are DELIBERATELY still admitted by
// every list this fix touches (see the BuiltinRegistry.hpp comments):
// `OfType` for fix #3559's diagnostic-routing reason (a bare/call reference
// should get the v0.284-removal wording, not a generic "no member");
// `Reversed`/`Sorted` because they are real Array<T> §4.4 native methods
// (not §5, but real). fix #3636 RETIRED `ForEach` from every list this fix
// touches: it is NOT a stdlib §5 instance member (the only documented
// `ForEach` is the unrelated static `Parallel.ForEach`), and the broken
// codegen path's malformed IR finally had to be retired, with a clean
// "no member ... (§22.5 / §4.x)" diagnostic taking its place. This file
// pins that none of the three remaining names regressed to the "no member"
// wording either.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Per tests/README.md: a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by exit code or
// `!r.ran` alone (a runtime crash also fails to "run").
bool hasErrorContaining(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

std::string arrayProgram(const char* callExpr) {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var r := ") + callExpr + std::string("; "
        "  return 0; "
        "end Main; end C;");
}

}  // namespace

// ── The five removed bogus names now get the PRECISE collection diagnostic,
//    not the generic #2457 fallback (FAIL-WITHOUT/PASS-WITH; commit's own
//    revert-and-rebuild measured the OLD wording before this fix) ─────────

BEZEL_TEST(linq_operator_name_list_dedup_3589, tolist_rejected_precise_stdlib_5) {
    // stdlib §5's own text: "There is no `ToList()`..." — never a real member.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        arrayProgram("a.ToList()"),
        "no member `ToList` on `Array<T>` (\xC2\xA7""22.5 / \xC2\xA7""4.x)"));
}

BEZEL_TEST(linq_operator_name_list_dedup_3589, append_rejected_precise_stdlib_3_1) {
    // `Append` is real, but on `StringBuilder` (stdlib §3.1), not on `Array<T>`.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        arrayProgram("a.Append(4)"),
        "no member `Append` on `Array<T>` (\xC2\xA7""22.5 / \xC2\xA7""4.x)"));
}

BEZEL_TEST(linq_operator_name_list_dedup_3589,
           sequenceequal_rejected_precise_stdlib_4_14) {
    // `SequenceEqual` is real, but on `ArraySpan<T>`/`ReadOnlyArraySpan<T>`
    // (stdlib §4.14/§4.15), not on `Array<T>` itself.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        arrayProgram("a.SequenceEqual(a)"),
        "no member `SequenceEqual` on `Array<T>` (\xC2\xA7""22.5 / \xC2\xA7""4.x)"));
}

BEZEL_TEST(linq_operator_name_list_dedup_3589, join_rejected_precise_stdlib_6_10) {
    // `Join` is real, but as `TaskScope.Join()` (stdlib §6.10) or the STATIC
    // `string.Join(sep, items)`, not an `Array<T>` instance method.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        arrayProgram("a.Join()"),
        "no member `Join` on `Array<T>` (\xC2\xA7""22.5 / \xC2\xA7""4.x)"));
}

BEZEL_TEST(linq_operator_name_list_dedup_3589,
           asreadonly_rejected_precise_stdlib_4_14) {
    // `AsReadOnly` is real, but on `ArraySpan<T>`/`Span<T>` (stdlib
    // §4.14/§4.15), not on `Array<T>` itself.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        arrayProgram("a.AsReadOnly()"),
        "no member `AsReadOnly` on `Array<T>` (\xC2\xA7""22.5 / \xC2\xA7""4.x)"));
}

// ── Deliberately-retained names must NOT regress to the "no member"
//    wording (they are real, or real-elsewhere-with-diagnostic-routing
//    reasons — see the file header) ─────────────────────────────────────

BEZEL_TEST(linq_operator_name_list_dedup_3589, reversed_still_admitted_stdlib_4_4) {
    // `Reversed()` is a real Array<T> native method (§4.4) — must still work,
    // not merely "not rejected by the wrong diagnostic".
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var rev := a.Reversed(); "
        "  return rev[0]; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 3);
}

BEZEL_TEST(linq_operator_name_list_dedup_3589, sorted_still_admitted_stdlib_4_4) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  using var s := a.Sorted(); "
        "  return s[0]; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(linq_operator_name_list_dedup_3589,
           oftype_not_generic_nomember_diagnostic) {
    // fix #3559 retired `OfType<U>()`'s LOWERING but deliberately kept its
    // name in every membership list so the diagnostic stays the specific
    // v0.284-removal wording, not a generic "no member". Confirm the
    // "no member" wording specifically does NOT fire (the removal wording
    // is a separate, dedicated diagnostic this test does not re-pin).
    BEZEL_EXPECT_FALSE(hasErrorContaining(
        arrayProgram("a.OfType<int>()"),
        "no member `OfType` on `Array<T>` (\xC2\xA7""22.5 / \xC2\xA7""4.x)"));
}
