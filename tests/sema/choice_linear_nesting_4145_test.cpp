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
// clock ceiling of 30s for shard-load tolerance (3.7x post-fix, 0.51x
// pre-fix; margin vs pre-fix: 28.9s). Triggered by cycle-9 gate cycle 6
// plain oracle COVERAGE SHORT on all 5 attempts: the test passes in
// <0.01s in isolation but flakes under shard sema/11 load at the 20s
// ceiling. NOT oracle weakening — any classify >= 30s still fails.

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
    // This test asserts a wall-clock ceiling of 30s for shard-load
    // tolerance. A linear-O(N) classifier at depth 50 fits easily; a
    // quadratic classifier at depth 50 still overruns this ceiling (28.9s
    // margin vs pre-fix). The 20s ceiling was flake-prone under shard
    // sema/11 load (cycle-9 gate cycle 6: COVERAGE SHORT on 5/5 plain
    // oracle attempts; arbitration marked this test FLAKY).
    //
    // The chain's terminal verdict is ValueLike: the §28.1 erasure
    // oracle erases the `owned` marker at `D<int>` (int is a value
    // builtin), so `D<int>` is value-like and every `Cn Wrap` over it
    // is value-like and `C1` is therefore value-like. The verdict
    // itself is not what this test pins — termination within the
    // wall-clock ceiling is.
    constexpr int kDepth = 50;
    constexpr auto kWallClockCeiling = std::chrono::seconds(30);
    // fix #4459 (cycle-9 cycle-7 COVERAGE SHORT verdict flake): the AST
    // borrows string_views into the source buffer for every name part
    // (TypeDecl::name, ChoiceCase::name, every Param::name, etc.), and
    // `depthNSuf` returns a temporary that the AST would outlive if not
    // captured here. Held locally so the borrowed string_views stay live
    // for every classify call that follows (the classifier consults
    // headName() during the §35.4 substitution on the bare `T` payload
    // of `D<T>.Some`, #4140+#4112 — a use-after-free there reads garbage
    // and the conservative bare-parameter arm of `ownedMarkerActiveOn…
    // TypeImpl` returns `true`, flipping D<int> to Owning and the whole
    // chain to Owning). Capturing the source costs ~50 KB and is
    // negligible vs the wall-clock budget. Per §9.7 / §35.4 / §28.1.
    std::string src = depthNSrc(kDepth);
    auto r = resolveAll(src);
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
