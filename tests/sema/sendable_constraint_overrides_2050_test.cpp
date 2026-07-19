// Spec-derived regression tests for issue #2050 (slice 2) — the §14.2
// "Derived-property constraints" admitted-set entry `where T: sendable`
// (v0.201, proposal 0049 train B): constraint INHERITANCE on overrides and
// non-regression on the existing stdlib-shape call-site sendability gates
// (Channel<T>, Domain.Spawn).
//
// §14.2 "Constraints on overrides and interface implementations" (v0.182,
// #1579): "An `override` of a generic method ... inherits the overridden
// ... declaration's constraints. Omitting the `where` clauses is the
// recommended spelling and means exactly the inherited set; the clauses may
// instead be restated in an equivalent form ... A differing clause — added,
// removed, narrowed, or widened relative to the inherited set — is a compile
// error." Slice 1 made `sendable` a constraint; this file verifies the
// existing inheritance machinery (constraintSetsEquivalent, kind-equality)
// handles the new kind correctly.
//
// Authority: spec §14.2 (v0.201 "Derived-property constraints" + v0.182
// "Constraints on overrides"); §30.14 sendable-type rules; stdlib §6.8 /
// §6.9 (Domain.Spawn / Channel<T> / ChannelSender<T>).

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::SemaResult;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff an override-restates-constraint diagnostic was reported.
bool hasOverrideConstraintError(const SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("restates a `where") != std::string::npos)
            return true;
    return false;
}

// Count diagnostics whose text names the §14.2 constraint failure (the
// constraint-failure diagnostic at the instantiation site). Used to verify
// the §14.2 "one diagnostic (the constraint failure, at the instantiation
// site) replaces the interior one" sharpening.
std::size_t countSendableConstraintErrors(const SemaResult& r) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") != std::string::npos &&
            d.message.find("sendable") != std::string::npos)
            ++n;
    return n;
}

}  // namespace

// ── ACCEPT: override inherits `where T: sendable` ───────────────────────────

// §14.2 / v0.182 (#1579): the override OMITS the `where` clause and so
// inherits exactly the base's `sendable` constraint. Clean-compile.
BEZEL_TEST(sendable_override_2050,
           override_omitted_clause_inherits_sendable_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method Send<T>(x: T): int "
        "  where T: sendable return 0; end Send; "
        "end Base; "
        "class Derived : Base public override method Send<T>(x: T): int "
        "  return 1; end Send; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(!hasOverrideConstraintError(r));
}

// §14.2 / v0.182 (#1579): the override RESTATES the identical clause —
// `where T: sendable` on both sides. Clean-compile.
BEZEL_TEST(sendable_override_2050,
           override_restates_identical_sendable_clause_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method Send<T>(x: T): int "
        "  where T: sendable return 0; end Send; "
        "end Base; "
        "class Derived : Base public override method Send<T>(x: T): int "
        "  where T: sendable return 1; end Send; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(!hasOverrideConstraintError(r));
}

// ── REJECT: a differing clause on the override is a compile error ────────────

// §14.2 / v0.182 (#1579): the override ADDS `class` to the inherited
// `sendable` set — a differing (narrowed) clause. Rejected with the §14.2
// override-restates-constraint diagnostic.
BEZEL_TEST(sendable_override_2050,
           override_adds_class_to_sendable_rejected_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method Send<T>(x: T): int "
        "  where T: sendable return 0; end Send; "
        "end Base; "
        "class Derived : Base public override method Send<T>(x: T): int "
        "  where T: sendable, class return 1; end Send; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(hasOverrideConstraintError(r));
}

// §14.2 / v0.182 (#1579): the override NARROWS by removing `sendable`
// entirely (no clause) when the base had `sendable, class` — a widened
// admission relative to the dispatch surface. Rejected.
BEZEL_TEST(sendable_override_2050,
           override_drops_sendable_from_compose_rejected_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public virtual method Send<T>(x: T): int "
        "  where T: sendable, class return 0; end Send; "
        "end Base; "
        "class Derived : Base public override method Send<T>(x: T): int "
        "  where T: class return 1; end Send; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(hasOverrideConstraintError(r));
}

// ── NON-REGRESSION: stdlib-shape sendable-element gates still fire ───────────

// §30.15 / §3.6 (#600): `new Channel<OpenClass>()` is rejected at the `new`
// site by the EXISTING §30.14 sendable-element check (untouched by the
// constraint work). The diagnostic names §30.14/§30.15 — the runtime gate,
// NOT the §14.2 constraint (there is no user `where T: sendable` clause here;
// the stdlib signature is enforced by the §30.14 runtime check).
BEZEL_TEST(sendable_override_2050,
           channel_open_class_element_still_rejected_at_new_site_30_15) {
    auto r = parseSemaAll(
        "module M; class OpenData public Val: int := 0; "
        "destructor end destructor; end OpenData; "
        "class C public static method Test() "
        "  using var ch := new Channel<OpenData>(); "
        "end Test; end C;");
    bool sawChannelReject = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is not sendable") != std::string::npos &&
            d.message.find("Channel") != std::string::npos)
            sawChannelReject = true;
    BEZEL_EXPECT_TRUE(sawChannelReject);
}

// §30.14 (#600): a Domain.Spawn argument that is a non-sendable class is
// rejected by the EXISTING call-site sendable gate (untouched). The
// diagnostic names §30.14 — the runtime gate, not §14.2 (no user `where T:
// sendable` here; the Domain.Spawn compiler-known signature is enforced by
// the §30.14 runtime check at the call).
BEZEL_TEST(sendable_override_2050,
           spawn_open_class_arg_still_rejected_at_call_site_30_14) {
    auto r = parseSemaAll(
        "module M; class OpenData public Val: int := 0; "
        "destructor end destructor; end OpenData; "
        "class P public static method Entry(owned d: OpenData) end Entry; "
        "end P; "
        "class C public static async method Main(): Task<int> "
        "  using var t := Domain.Spawn(P.Entry, new OpenData()); "
        "  await t; "
        "  return 0; "
        "end Main; end C;");
    bool sawSpawnReject = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("Spawn") != std::string::npos &&
            d.message.find("not sendable") != std::string::npos)
            sawSpawnReject = true;
    BEZEL_EXPECT_TRUE(sawSpawnReject);
}

// ── §14.2 "single diagnostic" sharpening ────────────────────────────────────

// §14.2 "Derived-property constraints" (v0.201): "the per-instantiation
// re-check for those interior uses is subsumed by the constraint, and one
// diagnostic (the constraint failure, at the instantiation site) replaces
// the interior one." A user generic constrained `where T: sendable`,
// instantiated with a non-sendable class, emits the constraint failure
// exactly once at the instantiation site (the body's `Channel<T>` use, if
// reached, would have added its own diagnostic — the constraint blocks
// instantiation first).
BEZEL_TEST(sendable_override_2050,
           user_generic_sendable_violation_emits_one_constraint_diag_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData public Val: int := 0; destructor end destructor; "
        "end OpenData; "
        "public class Mailbox<T> where T: sendable "
        "  public constructor() end constructor; "
        "end Mailbox; "
        "class C public static method Main(): int "
        "  using var mb := new Mailbox<OpenData>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(countSendableConstraintErrors(r), std::size_t{1});
}
