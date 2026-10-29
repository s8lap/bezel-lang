// Spec-derived regression for issue #7456: the §28.10 vacuity lint must not
// fire on a `using var` binding whose DECLARED TYPE is one of the built-in
// DISPOSAL INTERFACES.
//
// THE DEFECT
// ==========
// `spec §28.10` (v0.345) gives the vacuity lint: a `using var` whose
// right-hand side carries NO disposal obligation should be written without the
// disposal hint. The lint's premise is `spec §28.9`'s — "the RHS is a
// value-typed/runtime-managed value with no disposal obligation".
//
// `spec §8.7` (v0.345): "The compiler generates `IDisposable` on every
// non-static class." `spec §28.12` (v0.345): the disposal dispatches
// "through any interface — `IDisposable` or one deriving from it" to the
// runtime class.
//
// => A BINDING WHOSE DECLARED TYPE IS A DISPOSAL INTERFACE, OVER AN OBJECT
// THAT HAS A DESTRUCTOR, CARRIES A REAL OBLIGATION. The lint said it does
// not, and it was wrong.
//
// THE THREE VIEWS THAT DISAGREED
// ===============================
// The compiler held THREE hand-written views of "which interfaces are disposal
// interfaces", and they disagreed:
//
//   * the chain table `kDisposalHeadParents`
//     (src/sema/TypeResolution_Checks.cpp) knew all TEN;
//   * the §28.10 lint's table `kDisposableBuiltinHeadNames`
//     (src/common/BuiltinRegistry.cpp) knew SIX — missing `IDisposable`,
//     `IAsyncDisposable`, `IOwningCollection<T>` and `IOwningList<T>`;
//   * the bound check `primitiveNeverConformsToBuiltinIface`
//     (src/sema/TypeRef.hpp) knew TWO — `IDisposable` and
//     `IAsyncDisposable`.
//
// The four heads the lint's table missed are EXACTLY the four that
// over-fired. The family is ONE table now
// (`common::isBuiltinDisposalFamilyHead`, src/common/BuiltinRegistry.cpp),
// read by all three sites.
//
// THE ADMITTING-TWIN MATRIX (six cells — §3.3 of the review contract)
// ==============================================================
// The five sibling heads that NEVER warned must not START warning. A fix
// that silences the lint over the whole family would pass the four
// over-firing rows and be a REGRESSION on the five; the twin rows below are
// part of the test, not an afterthought.
//
//   head                     before      after
//   ------------------------ ----------- ----------
//   IEnumerator<Res>         NO warning  NO warning
//   IOwningEnumerator<Res>   NO warning  NO warning
//   IOwningEnumerable<Res>   NO warning  NO warning
//   IAsyncEnumerator<Res>    NO warning  NO warning
//   IOwningAsyncEnumerator<Res> NO warning NO warning
//   ------------------------ ----------- ----------
//   IOwningCollection<Res>   WARNING     NO warning   <- over-fired
//   IOwningList<Res>         WARNING     NO warning   <- over-fired
//   IDisposable              WARNING     NO warning   <- over-fired
//   IAsyncDisposable         WARNING     NO warning   <- over-fired
//
// THE RUNTIME ORACLE
// ==================
// The obligation is REAL, so the program must still print `RES-DISPOSED`.
// The lint's silence is not "there is nothing to dispose" — the destructor
// runs.
//
// THE CONTROL
// ===========
// A genuinely vacuous `using var` (over a value-typed right-hand side) MUST
// still draw the warning: the fix must not silence the lint wholesale.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// True iff some WARNING diagnostic fires the inert-RHS vacuity message for
// `name` — "`using var` `x` is vacuous — the RHS is a value-typed/
// runtime-managed value with no disposal obligation (§28.9)".
bool hasInertRhsWarning(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("RHS is a value-typed/runtime-managed") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff some WARNING diagnostic fires the vacuous-BORROW-PIN message for
// `name` — "`using var` `x` pins a source that already outlives this scope".
// This is Case B of the same lint, reached when Case A's premise does not
// hold. A fix that only removed the Case A premise would move the warning
// here rather than remove it, so the twin rows assert BOTH are silent.
bool hasVacuousBorrowPinWarning(const OwnershipResult& r,
                                const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("pins a source that already outlives") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// The declared type of the binding for a family head: the two non-generic
// heads are spelled bare, the eight generic ones take the element type.
std::string declaredTypeFor(const std::string& head) {
    if (head == "IDisposable" || head == "IAsyncDisposable") return head;
    return head + "<Res>";
}

// The alias form the issue measures: a destructor-bearing `Res` inside an
// `OwningArray`, bound at the given disposal head.
//
// `using var c: <head> := src;` — the UNMARKED read of a named binding. Over a
// named OWNED class or interface binding that spelling is a compile error per
// `spec §28.10` v0.350 (*Adopting a named owned binding*), so `src` here is a
// BORROWING ALIAS of the owner (`var src := owner;`, §28.3): the unmarked read
// of a borrow-state binding is the borrow-pin the vacuity lint talks about. (Through v0.345 this program bound a LOCAL and
// the compiler admitted it, issue #5582, the divergence corpus row
// `s28_10_0015` carried; issue #7481 closed it, so the source moved to a
// borrowing alias to keep the lint question this lane pins askable.) What this
// lane pins is that the §28.10 VACUITY warning — a claim that the RHS
// carries NO disposal obligation — is not drawn over a disposal-interface
// binding whose object HAS a destructor.
//
// The element type is `Res`, a CLASS with a destructor, so the obligation the
// lint denies is real and is discharged (by the owner of `src`).
std::string aliasProgram(const std::string& head) {
    return
        "module M; "
        "public class Res "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"RES-DISPOSED\"); end destructor; "
        "end Res; "
        "public class Program "
        "  public static method Main() "
        "    using var owner := new OwningArray<Res>(); "
        "    owner.Add(new Res()); "
        "    var src := owner; "
        "    using var c: " + declaredTypeFor(head) + " := src; "
        "    Console.WriteLine(\"in-scope\"); "
        "  end Main; "
        "end Program; ";
}

// The MARKED form: `using var c: <head> := move src;` adopts the owned
// value, so the scope-end disposal is the binding's own obligation and the
// destructor runs at scope end. This is the RUNTIME oracle — the program
// prints `RES-DISPOSED`.
std::string moveProgram(const std::string& head) {
    return
        "module M; "
        "public class Res "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"RES-DISPOSED\"); end destructor; "
        "end Res; "
        "public class Program "
        "  public static method Main() "
        "    var src := new OwningArray<Res>(); "
        "    src.Add(new Res()); "
        "    using var c: " + declaredTypeFor(head) + " := move src; "
        "    Console.WriteLine(\"in-scope\"); "
        "  end Main; "
        "end Program; ";
}

// The four heads the lint's table missed (#7456's own rows).
const std::vector<std::string>& overFiredHeads() {
    static const std::vector<std::string> kHeads = {
        "IOwningCollection", "IOwningList", "IDisposable", "IAsyncDisposable"};
    return kHeads;
}

// The five sibling heads that never warned — the admitting twins.
const std::vector<std::string>& admittingTwinHeads() {
    static const std::vector<std::string> kHeads = {
        "IEnumerator", "IOwningEnumerator", "IOwningEnumerable",
        "IAsyncEnumerator", "IOwningAsyncEnumerator"};
    return kHeads;
}

}  // namespace

// ============================================================================
// #7456's own rows — the four heads that over-fired.
// ============================================================================

// per §28.9 / §28.10 / §28.12 / §8.7, fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           io_owning_collection_declared_binding_does_not_warn_28_10) {
    auto r = parseAndCheckOwnership(aliasProgram("IOwningCollection"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// per §28.9 / §28.10 / §28.12 / §8.7, fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           io_owning_list_declared_binding_does_not_warn_28_10) {
    auto r = parseAndCheckOwnership(aliasProgram("IOwningList"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// per §28.9 / §28.10 / §28.12 / §8.7, fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           idisposable_declared_binding_does_not_warn_28_10) {
    auto r = parseAndCheckOwnership(aliasProgram("IDisposable"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// per §28.9 / §28.10 / §28.12 / §8.7, fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           i_async_disposable_declared_binding_does_not_warn_28_10) {
    auto r = parseAndCheckOwnership(aliasProgram("IAsyncDisposable"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// ============================================================================
// THE ADMITTING-TWIN MATRIX — the five siblings that never warned must not
// START warning. Each is a separate row so a regression names the head.
// ============================================================================

// per §28.10 / §29.1 (stdlib §4.12), fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           twin_enumerator_declared_binding_does_not_warn_28_10) {
    auto r = parseAndCheckOwnership(aliasProgram("IEnumerator"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// per §28.10 / §29.1 (stdlib §4.13), fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           twin_owning_enumerator_declared_binding_does_not_warn_28_10) {
    auto r = parseAndCheckOwnership(aliasProgram("IOwningEnumerator"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// per §28.10 / §29.1 (stdlib §4.13), fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           twin_owning_enumerable_declared_binding_does_not_warn_28_10) {
    auto r = parseAndCheckOwnership(aliasProgram("IOwningEnumerable"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// per §28.10 / §30.7 (stdlib §4.12), fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           twin_async_enumerator_declared_binding_does_not_warn_28_10) {
    auto r = parseAndCheckOwnership(aliasProgram("IAsyncEnumerator"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// per §28.10 / §30.7 (stdlib §4.13), fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           twin_owning_async_enumerator_declared_binding_does_not_warn_28_10) {
    auto r = parseAndCheckOwnership(aliasProgram("IOwningAsyncEnumerator"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// ============================================================================
// THE RUNTIME ORACLE — the obligation is REAL: the destructor runs at scope
// end, so the program prints `RES-DISPOSED`.
// ============================================================================

// per §28.3 / §28.4 / §28.10 / §28.12, fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           io_owning_collection_binding_still_disposes_28_4) {
    JitResult r = runAot(moveProgram("IOwningCollection"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("RES-DISPOSED") != std::string::npos);
}

// per §28.3 / §28.4 / §28.10 / §28.12, fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           idisposable_binding_still_disposes_28_4) {
    JitResult r = runAot(moveProgram("IDisposable"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("RES-DISPOSED") != std::string::npos);
}

// per §28.3 / §28.4 / §28.10 / §28.12, fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           io_owning_list_binding_still_disposes_28_4) {
    JitResult r = runAot(moveProgram("IOwningList"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("RES-DISPOSED") != std::string::npos);
}

// ============================================================================
// THE CONTROL — a genuinely vacuous `using var` (a value-typed right-hand
// side) MUST still draw the warning. The fix must not silence the lint.
// ============================================================================

// per §28.9 / §28.10, fix #7456
BEZEL_TEST(disposal_family_vacuity_lint_7456,
           control_value_typed_using_var_still_warns_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Program "
        "  public static method Main() "
        "    using var x := 5; "
        "    Console.WriteLine(\"in-scope\"); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}
