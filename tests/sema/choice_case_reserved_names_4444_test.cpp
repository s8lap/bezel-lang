// fix #4444 / §35.1 (v0.302): a choice case named `Equals`, `GetHashCode`,
// or `ToString` is reserved against case names on every classification —
// not only on the choices where §35.7 synthesizes them. v0.300's changelog
// claimed "no case-vs-member collision check was located in `src/sema/` at
// all", which was wrong (the §11.3 walker in `checkTypeDuplicates` already
// catches a case declared alongside a member of the same name); the actual
// gap was the synthesized/inherited names that never appear in
// `TypeSymbol::members`. #4444 closes that gap with a choice-specific
// reservation beside the duplicate walk: a `ChoiceCaseSymbol` whose name
// matches the reservation is REJECTED regardless of classification and
// regardless of whether a member of that name is declared. Keying it on
// classification is explicitly declined (§35.1 DECLINED.md, v0.302): the
// use-site compile error the §35.7 auto-package removes exists on exactly
// the classifications where synthesis does NOT fire, so the reservation
// must hold there precisely.
//
// Tests below cover the three shapes owed coverage in v0.302's changelog:
//   * owning choice (owned payload) with a `ToString` case
//   * borrow-bearing choice (plain class payload) with an `Equals` case
//   * value-like auto-hashable choice (payloads of int only) with a
//     `GetHashCode` case
// Each is REJECTED with the §35.1 / §35.7 diagnostic naming the remedy.

#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// fix #3985: gated helper that mirrors the real driver's
// parse → collect → resolve → typecheck staging. The §35.1 reservation
// fires inside `resolveTypes` (alongside `checkTypeDuplicates`), so a
// `parseAndResolveGated` would suffice — but typechecking confirms the
// rejection survives the full sema pass and is not just a parse-stage
// accident.
bezel::test::SemaResult semaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

bool hasErrDiagWith(const bezel::test::SemaResult& r,
                    std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// =========================================================================
// §35.1 / #4444 — REJECTION SHAPES (one per classification)
// =========================================================================

// §35.1 / #4444: an OWNING choice (a payload field marked `owned`) with a
// case named `ToString` is rejected. `ToString` is reserved against case
// names on every classification, so even outside the value-like set where
// §35.7's synthesis fires, the case holding the name would put the §35.7
// stated remedy (declare the member) out of reach.
BEZEL_TEST(choice_case_reserved_names_4444,
           spec_35_1_owning_choice_ToString_case_rejected_issue_4444) {
    auto r = semaAll(
        "module M; "
        "class Connection end Connection; "
        "choice C "
        "  ToString, "
        "  Other(owned conn: Connection); "
        "end C; "
        "class Driver public static method Main(): int return 0; "
        "end Main; end Driver;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "case `ToString`"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "reserved against case names"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "§35.1"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "§35.7"));
    // The diagnostic must NAME the remedy (CLAUDE.md / #4444 issue body).
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "rename the case"));
}

// §35.1 / #4444: a BORROW-BEARING choice (a class-typed payload, no
// `owned`) with a case named `Equals` is rejected. The reservation holds
// on every classification, so a borrow-bearing choice (where §35.7 does
// NOT synthesize the trio) is also covered.
BEZEL_TEST(choice_case_reserved_names_4444,
           spec_35_1_borrow_bearing_choice_Equals_case_rejected_issue_4444) {
    auto r = semaAll(
        "module M; "
        "class Logger end Logger; "
        "choice C "
        "  Equals, "
        "  Borrowed(log: Logger); "
        "end C; "
        "class Driver public static method Main(): int return 0; "
        "end Main; end Driver;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "case `Equals`"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "reserved against case names"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "§35.1"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "§35.7"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "rename the case"));
}

// §35.1 / #4444: a VALUE-LIKE AUTO-HASHABLE choice (payloads of int
// only) with a case named `GetHashCode` is rejected. This is the very
// classification where §35.7 synthesizes the trio — the duplicate-walk
// branch (`checkTypeDuplicates`) would NOT fire here because no member of
// that name is declared, so the rejection must come from the
// §35.1-reservation branch specifically.
BEZEL_TEST(choice_case_reserved_names_4444,
           spec_35_1_value_like_choice_GetHashCode_case_rejected_issue_4444) {
    auto r = semaAll(
        "module M; "
        "choice C "
        "  GetHashCode(value: int), "
        "  Other; "
        "end C; "
        "class Driver public static method Main(): int return 0; "
        "end Main; end Driver;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "case `GetHashCode`"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "reserved against case names"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "§35.1"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "§35.7"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "rename the case"));
}

// =========================================================================
// §35.1 / #4444 — ACCEPT SHAPES (false-reject regression guard)
// =========================================================================

// §35.1 / #4444: a choice with cases whose names do NOT collide with the
// reservation is accepted. The §35.1 reservation is exactly the three
// names — no widening — so a case like `MyCase` is unaffected and the
// choice passes sema without diagnostic.
BEZEL_TEST(choice_case_reserved_names_4444,
           spec_35_1_no_reserved_case_names_accepted_issue_4444) {
    auto r = semaAll(
        "module M; "
        "choice C "
        "  Red, "
        "  Green, "
        "  Blue; "
        "end C; "
        "class Driver public static method Main(): int return 0; "
        "end Main; end Driver;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
