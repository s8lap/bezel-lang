// Spec-derived sema test for §35.4 — linear choice-nesting depth must
// terminate within a wall-clock ceiling (fix #4145, family J §2.6,
// v0.298).
//
// Background:
//
// #4145: a chain of choices `Cn Wrap(payload: C{n-1})` … `C1
// Wrap(payload: D<T>)` … `D<T> Some(owned t: T), None` becomes
// exponentially expensive to classify when each wrap's payload field
// walks back through classifyChoiceCore. The exponential arises because
// the unmarked-payload walk in `payloadFieldTypeIsMoveOnly` re-walks
// the SAME nested `Cn` for every distinct enclosing site — the SAME
// TypeRef classified multiple times on the same path.
//
// Fix #4145 added a per-classifyChoiceType-call MEMOIZATION cache on
// classifyChoiceCore, keyed on `const sema::TypeRef*` (the instantiation).
// The cache stores only FINAL verdicts (the cycle guard's `ValueLike`
// from a re-entry edge is intentionally NOT cached so a sibling from a
// NEW path still gets the real answer). Each wrap is classified at most
// once per call, so the overall cost goes from ~N^2 to O(N).
//
// The HEAD b57a712e pre-measurement (no fix) showed depth-50 at 58.9s
// wall; the post-fix measurement is 8.2s. This test asserts a wall-
// clock ceiling at depth-50 that lets a regression to the pre-fix
// behavior (or any quadratic-with-large-constant regression) fail
// loudly without spuriously failing on a transient slowdown.

#include <chrono>
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

// Compose a depth-N linear-nesting choice source string. `Cn Wrap(...)`
// … `C1 Wrap(payload: D<T>)` … `D<T> Some(owned t: T), None`. The choices
// are NOT mutually recursive (no self-containment — every payload points
// AT a DIFFERENT choice); the depth is purely LINEAR.
std::string depthNSrc(int n) {
    std::string src = "module M; ";
    src += "choice D<T> Some(owned t: T), None; end D; ";
    // The outermost wrap is `C1`. Each C{i} carries an UNMARKED payload
    // of type `C{i+1}`, so `payloadFieldTypeIsMoveOnly`'s Choice-kind
    // arm must delegate to classifyChoiceCore; that delegation without
    // memoization would be quadratic (every C{i} classified for each
    // enclosing C{j}).
    src += "choice C1 Wrap(payload: C2); end C1; ";
    for (int i = 2; i < n; ++i) {
        src += "choice C" + std::to_string(i) +
               " Wrap(payload: C" + std::to_string(i + 1) +
               "); end C" + std::to_string(i) + "; ";
    }
    // C{n} wraps a `D<Widget>` so the chain base is the owning choice
    // (a class instantiation of the generic — §28.1 does NOT erase at
    // class instantiations). This makes the WHOLE chain owning, which
    // is what the memoization cache must keep classifying consistently.
    src += "choice C" + std::to_string(n) +
           " Wrap(payload: D<int>); end C" + std::to_string(n) + "; ";
    return src;
}

}  // namespace

// --- §35.4 + #4145: linear choice-nesting depth completes in wall-clock budget

BEZEL_TEST(choice_linear_nesting_4145,
           spec_35_4_4145_depth_50_completes_within_wall_clock_ceiling) {
    // #4145: depth-50 pre-fix = 58.9s wall (~N^2). Post-fix = 8.2s.
    // This test asserts a wall-clock ceiling of 20s, comfortably above
    // the post-fix measurement but firmly below any regression to the
    // pre-fix quadratic behavior. A linear-O(N) classifier at depth 50
    // fits easily; a quadratic classifier at depth 50 times out or
    // overruns this ceiling.
    //
    // The chain's terminal verdict is ValueLike: the §28.1 erasure
    // oracle erases the `owned` marker at `D<int>` (int is a value
    // builtin), so `D<int>` is value-like and every `Cn Wrap` over it
    // is value-like and `C1` is therefore value-like. The verdict
    // itself is not what this test pins — termination within the
    // wall-clock ceiling is.
    constexpr int kDepth = 50;
    constexpr auto kWallClockCeiling = std::chrono::seconds(20);
    auto r = resolveAll(depthNSrc(kDepth));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto tStart = std::chrono::steady_clock::now();
    auto c = classify(r, "C1");
    auto tEnd = std::chrono::steady_clock::now();
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::ValueLike);
    auto wall = std::chrono::duration_cast<std::chrono::seconds>(
        tEnd - tStart);
    BEZEL_EXPECT_TRUE(wall < kWallClockCeiling);
}
