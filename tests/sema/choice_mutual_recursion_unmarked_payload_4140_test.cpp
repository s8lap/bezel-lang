// Spec-derived sema tests for §35.4 — mutually-recursive UNMARKED choice
// pairs terminate without stack overflow (fix #4140, family J §2.6,
// v0.298).
//
// Background:
//
// #4140 stack walk: `payloadFieldTypeIsMoveOnly`'s Choice arm used to
// seed a FRESH `nestedChoiceSeen` set on every recursive
// `classifyChoiceCore` call, severing the path-guard chain the
// surrounding classifyChoiceCore call was maintaining. A mutually-
// recursive UNMARKED choice pair — `choice A X(t: B); choice B Y(t: A);`
// — recursed without limit (78,510 frames in #4140's captured stack). The
// orphaned frame on return is the §35.1 self-containment diagnostic the
// program should already have produced.
//
// The fix: thread the AMBIENT `choiceSeen` through the recursion (mirroring
// `payloadFieldTypeIsBorrowCarrying`'s own Choice-kind arm), re-key
// `owningSeen` on `const sema::TypeRef*` (instantiation, not declaration,
// mirroring #3933's earlier re-keying of classifyChoiceCore's own guard),
// and add a per-classifyChoiceType-call MEMOIZATION cache on
// classifyChoiceCore that collapses the re-walks the unmarked-payload
// leg would otherwise produce.
//
// The §35.1 self-containment diagnostic must be reached BEFORE any further
// classification attempt — the cycle guard (insert on entry, erase on
// exit, return `ValueLike` on cycle) is the mechanism that closes this
// requirement: a re-entry on the current path terminates with `ValueLike`
// rather than stack-overflowing, and the memoization cache stores only
// FINAL verdicts — never the cycle guard's `ValueLike` — so a follow-up
// classification from a DIFFERENT path still gets the real verdict.
//
// Companion: the partner test in choice_classification_35_4_test.cpp —
// `spec_35_4_mutually_recursive_choice_pair_via_owned_marker_no_crash` —
// covers the OWNED-marker form (where the cycle enters through
// ownedMarkerActiveOnInstantiatedTypeImpl). This file covers the UNMARKED
// form (#4140's headline), which enters through payloadFieldTypeIsMoveOnly.

#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "common/OwnershipFacts.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::common::ChoiceClassification;
using bezel::common::classifyChoiceType;
using bezel::sema::Symbol;
using bezel::sema::TypeSymbol;

namespace {

// Parse + collect + resolve (mirrors choice_classification_35_4_test.cpp).
bezel::test::SemaResult resolveAll(std::string_view src) {
    return bezel::test::parseAndResolveGated(src);
}

const TypeSymbol* findType(const bezel::test::SemaResult& r,
                           std::string_view name) {
    if (!r.ctx || !r.ctx->module()) return nullptr;
    for (Symbol* child : r.ctx->module()->children) {
        if (auto* t = dynamic_cast<TypeSymbol*>(child)) {
            if (t->name == name) return t;
        }
    }
    return nullptr;
}

std::optional<ChoiceClassification> classify(
    const bezel::test::SemaResult& r, std::string_view name) {
    auto* ts = findType(r, name);
    if (!ts || !r.ctx) return std::nullopt;
    bezel::sema::TypeRef rt;
    rt.kind = bezel::sema::TypeRef::Kind::Named;
    rt.declaration = const_cast<TypeSymbol*>(ts);
    return classifyChoiceType(&rt, *r.ctx);
}

}  // namespace

// --- §35.4 + §35.1: mutual recursion through UNMARKED payload terminates ----

BEZEL_TEST(choice_mutual_recursion_unmarked_payload_4140,
           spec_35_4_4140_mutual_pair_through_unmarked_payload_no_crash) {
    // #4140 headline shape: `A X(t: B); B Y(t: A);` — a mutually-recursive
    // UNMARKED choice pair. Pre-fix the classifier overflowed the stack
    // (78,510 frames in the issue's captured trace, deep in
    // classifyChoiceCore's recursive descent). The §35.1 self-containment
    // diagnostic (sema-side, separate pass) rejects the program; this test
    // asserts only that the classifier itself does NOT stack-overflow on
    // the cyclic value-typed walk — the cycle guard terminates the
    // recursion with `ValueLike`, and the verdict is whatever the
    // genuinely-classifiable component of the walk yields.
    auto r = resolveAll(
        "module M; "
        "choice A X(t: B); end A; "
        "choice B Y(t: A); end B;");
    // The §35.1 self-containment diagnostic IS expected to fire on this
    // cyclic input — the sema self-containment check is a separate
    // pass that rejects these programs outright. The classifier itself
    // is the system under test here; the assertion is only that the
    // classifier's classification call terminates rather than stack-
    // overflowing, NOT that there are zero diagnostics on the program.
    // (Pre-fix #4140, the classifier recursed without bound — the
    // framework detected the runaway via a process-level stack-overflow
    // signal, not via any check we can write here.)
    auto c = classify(r, "A");
    // Termination is the contract. Pre-fix this never returned; post-fix
    // it completes normally and yields a classification (the ValueLike
    // returned by the cycle guard, which is the expected verdict for the
    // already-rejected-by-§35.1 program).
    BEZEL_EXPECT_TRUE(c.has_value());
}

BEZEL_TEST(choice_mutual_recursion_unmarked_payload_4140,
           spec_35_4_4140_triple_chain_through_unmarked_payload_no_crash) {
    // #4140 stress: three choices forming an A → B → C → A cycle through
    // UNMARKED payloads. The memoization cache (the SAME cache the
    // classifier maintains across the recursion) collapses the
    // exponential re-walk the unmarked-payload leg would otherwise
    // produce; the cycle guard catches the re-entry edge. Both must
    // work together.
    auto r = resolveAll(
        "module M; "
        "choice A X(t: B); end A; "
        "choice B Y(t: C); end B; "
        "choice C Z(t: A); end C;");
    // The §35.1 self-containment diagnostic fires on the
    // mutually-recursive component graph (the cycle through A → B →
    // C → A). The classifier MUST terminate; it can answer with any
    // verdict, including `ValueLike` from the cycle guard. See the
    // companion test above for the rationale.
    auto c = classify(r, "C");
    BEZEL_EXPECT_TRUE(c.has_value());
}
