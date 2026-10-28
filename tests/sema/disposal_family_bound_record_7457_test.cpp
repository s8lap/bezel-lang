// Spec-derived regression for issue #7457: a `where` bound naming a member of
// the built-in DISPOSAL FAMILY must not admit a RECORD type argument.
//
// THE DEFECT
// ==========
// `spec §9.7` (v0.345, docs/Bezel_Language_Spec.md:1409):
//
//   "Records cannot have destructors, `owned` fields, or implement
//    `IDisposable` — directly or through an interface extending it,
//    `IAsyncDisposable` among them."
//
// `spec §14.2`'s worked example (`where T: IDisposable`, v0.345) names the
// closure the conformance rule reads:
//
//   "an interface conforms exactly when it extends `IDisposable` (rule 3):
//    `IAsyncDisposable`, `IEnumerator<T>`, `IAsyncEnumerator<T>`, and any
//    user-defined interface that lists `IDisposable` in its extension clause"
//
// stdlib §2.10 (v0.345) states the same for the bound:
//
//   "The constraint `where T: IDisposable` admits every class …, no records
//    (records cannot implement `IDisposable` per language spec §9.7), and the
//    interfaces that extend `IDisposable` directly or transitively —
//    including `IAsyncDisposable`, `IEnumerator<T>`, `IAsyncEnumerator<T>`."
//
// => A RECORD MUST NOT SATISFY `where T: IEnumerator`. IT DID.
//
// THE BUG WAS BROADER THAN THE ISSUE'S TABLE
// ==========================================
// The issue measured the bound with a RECORD as the bound interface's OWN type
// argument; in that shape the six owning/iteration heads were rejected — but
// by §14.4's class-only owning-generic restriction, not by the disposal rule.
// With the bound interface's own type argument a CLASS (the shape the issue's
// own repro's `Res` is meant to be), the SAME SIX were ADMITTED with a record
// type argument, exactly like `IEnumerator<R>` and `IAsyncEnumerator<R>`.
// The rows below pin BOTH shapes.
//
// THE SITE
// ========
// `src/sema/TypeRef.hpp`'s `primitiveNeverConformsToBuiltinIface` named only
// `IDisposable` and `IAsyncDisposable` — the SAME two-name narrowing the
// #7424 lane fixed in the CHAIN table, in a second file. The family is ONE
// table now (`common::isBuiltinDisposalFamilyHead`,
// src/common/BuiltinRegistry.cpp), read by this check, by the §28.10 vacuity
// lint, and by the declaration-site chain table.
//
// THE ADMITTING TWINS
// ===================
// `where T: IDisposable` and `where T: IAsyncDisposable` were already
// REJECTED for a record and must STAY rejected; and a CLASS type argument
// must still be ADMITTED against every family head — the fix must not
// over-reject.

#include <string>
#include <string_view>

#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::SemaResult;

namespace {

// True iff some ERROR-severity diagnostic contains `needle`.
bool hasErrorMessage(const SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The bound's declared type for a family head: the two non-generic heads are
// spelled bare, the eight generic ones take an element type.
std::string boundTypeFor(const std::string& head, const std::string& elem) {
    if (head == "IDisposable" || head == "IAsyncDisposable") return head;
    return head + "<" + elem + ">";
}

// The program: a generic method whose type parameter `T` is bounded at a
// disposal head, and a call site that supplies a RECORD type argument.
//
// `elem` is the bound interface's OWN type argument — a CLASS (`Res`) or a
// RECORD (`R`). `arg` is the type argument supplied for `T`.
std::string boundProgram(const std::string& head, const std::string& elem,
                         const std::string& arg) {
    return
        "module M; "
        "public record R end R; "
        "public class Res "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"RES-DISPOSED\"); end destructor; "
        "end Res; "
        "public class Box<T> public constructor(v: T) end constructor; end Box; "
        "public class Program "
        "  public static method Use<T>(b: Box<T>) "
        "    where T: " + boundTypeFor(head, elem) + " "
        "  end Use; "
        "  public static method Main() "
        "    Use(new Box<" + arg + ">(new " + arg + "())); "
        "    Console.WriteLine(\"hi\"); "
        "  end Main; "
        "end Program; ";
}

}  // namespace

// ============================================================================
// #7457's own rows — the two heads the issue names.
//
// The bound interface's own type argument is a CLASS (`Res`), the shape in
// which EVERY family head wrongly admitted a record.
// ============================================================================

// per §9.7 / §14.2 (stdlib §2.10), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           ienumerator_bound_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IEnumerator", "Res", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "does not satisfy the constraint `where T: IEnumerator`"));
}

// per §9.7 / §14.2 (stdlib §2.10), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           i_async_enumerator_bound_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IAsyncEnumerator", "Res", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "does not satisfy the constraint `where T: IAsyncEnumerator`"));
}

// ============================================================================
// THE SIX OWNING/ITERATION HEADS — the same shape, so the same rule.
//
// Before the fix these were ADMITTED (the §14.4 class-only owning-generic
// restriction does not fire here: the bound's own type argument is a CLASS).
// They must be REJECTED by the §9.7 disposal rule.
// ============================================================================

// per §9.7 / §14.2 / §14.4 (stdlib §4.13), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           owning_enumerable_bound_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IOwningEnumerable", "Res", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "does not satisfy the constraint `where T: IOwningEnumerable`"));
}

// per §9.7 / §14.2 / §14.4 (stdlib §4.13), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           owning_enumerator_bound_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IOwningEnumerator", "Res", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "does not satisfy the constraint `where T: IOwningEnumerator`"));
}

// per §9.7 / §14.2 / §14.4 (stdlib §4.13), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           owning_async_enumerable_bound_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IOwningAsyncEnumerable", "Res", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r,
        "does not satisfy the constraint `where T: IOwningAsyncEnumerable`"));
}

// per §9.7 / §14.2 / §14.4 (stdlib §4.13), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           owning_async_enumerator_bound_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IOwningAsyncEnumerator", "Res", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(
            r,
            "does not satisfy the constraint `where T: IOwningAsyncEnumerator`"));
}

// per §9.7 / §14.2 / §14.4 (stdlib §4.2), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           owning_collection_bound_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IOwningCollection", "Res", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "does not satisfy the constraint `where T: IOwningCollection`"));
}

// per §9.7 / §14.2 / §14.4 (stdlib §4.2), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           owning_list_bound_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IOwningList", "Res", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "does not satisfy the constraint `where T: IOwningList`"));
}

// ============================================================================
// THE ADMITTING TWINS — `IDisposable` and `IAsyncDisposable` were already
// REJECTED for a record and must STAY rejected.
// ============================================================================

// per §9.7 / §14.2 (stdlib §2.10), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           idisposable_bound_still_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IDisposable", "Res", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "does not satisfy the constraint `where T: IDisposable`"));
}

// per §9.7 / §14.2 (stdlib §2.10), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           i_async_disposable_bound_still_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IAsyncDisposable", "Res", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "does not satisfy the constraint `where T: IAsyncDisposable`"));
}

// ============================================================================
// THE OTHER SHAPE — the bound interface's OWN type argument is a RECORD.
//
// The six owning heads are rejected HERE by §14.4's class-only owning-generic
// restriction (the bound's own `where T: class`), which is a correct
// rejection for a different reason. The two heads the fix names, and the two
// non-owning enumeration heads, are rejected by the §9.7 disposal rule. All
// ten must reject.
// ============================================================================

// per §9.7 / §14.2 / §14.4, fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           record_element_bound_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IEnumerator", "R", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "does not satisfy the constraint `where T: IEnumerator`"));
}

// per §9.7 / §14.2 / §14.4, fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           record_element_owning_enumerable_bound_rejects_a_record_9_7) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IOwningEnumerable", "R", "R"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ============================================================================
// THE CONTROLS — a CLASS type argument must still be ADMITTED against every
// family head. The fix must not over-reject.
// ============================================================================

// per §14.2 (worked example: `where T: IDisposable`), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           control_class_argument_is_admitted_14_2) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IEnumerator", "Res", "Res"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// per §14.2 (worked example: `where T: IDisposable`), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           control_class_argument_is_admitted_for_owning_14_2) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IOwningEnumerable", "Res", "Res"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// per §14.2 (worked example: `where T: IDisposable`), fix #7457
BEZEL_TEST(disposal_family_bound_record_7457,
           control_class_argument_is_admitted_for_idisposable_14_2) {
    auto r = bezel::test::parseAndTypecheckGated(
        boundProgram("IDisposable", "Res", "Res"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
