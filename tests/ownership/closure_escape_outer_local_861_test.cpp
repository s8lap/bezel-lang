// Spec-derived tests for issue #861 — reject a closure that captures an owned
// scope-bound local and ESCAPES by being assigned to a local in an ENCLOSING
// block.
//
// Spec basis. §28.3 / §28.9: a closure that captures an owned local captures a
// BORROW whose lifetime is the local's block; the closure may not outlive that
// block. When the closure is assigned to a local declared in an enclosing block
// (which outlives the captured local), calling the closure after the inner block
// exits reads a disposed value — a use-after-dispose / SIGSEGV. The compiler
// previously accepted this and crashed at runtime.
//
// The escape-to-FIELD routes (`obj.f := () => r.V`, `this.f := () => r.V`) were
// already rejected by checkBorrowStoreEscape; this adds the escape-to-OUTER-LOCAL
// route. Soundness rests on a block-depth comparison: at the assignment both the
// target and the captured source are in lexical scope, so their declaring blocks
// lie on a single nesting path; the target outlives the source iff its block is
// strictly shallower (smaller depth). A same- or inner-block target is NOT an
// escape and stays accepted.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::OwnershipResult;

namespace {

// A disposable resource class R (its destructor makes it an owned scope-bound
// local) plus a holder class C with the given Main body.
std::string wrap(const std::string& mainBody) {
    return std::string(
        "module M; "
        "class R public V: int; public constructor(v: int) V := v; end; "
        "  destructor V := 0; end; end R; "
        "class C public static method Main(): int ") + mainBody +
        " end Main; end C;";
}

// True iff some diagnostic is the #861 reject-closure-escape-to-outer-local
// error: it cites §28.3 / §28.9 and names the captured source.
bool rejectsOuterLocalEscape(const OwnershipResult& r, std::string_view srcName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.9") == std::string::npos) continue;
        if (d.message.find("closure capturing") == std::string::npos) continue;
        if (d.message.find(std::string(srcName)) == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — a closure capturing an owned inner local, assigned to an enclosing
// local, is rejected.
// =============================================================================

// §28.3 / §28.9: `f` is a method-scope local; `r` is a using-var in an inner
// do-end. `f := () => r.V` lets the closure (held by `f`) outlive `r`. Rejected.
// This is the exact #861 repro that compiled and SEGFAULTED before the fix.
BEZEL_TEST(closure_escape_outer_local_861, escape_to_method_scope_local_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var f: () -> int := () => -1; "
        "do "
        "  using var rr := new R(55); "
        "  f := () => rr.V; "
        "end; "
        "return f(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsOuterLocalEscape(r, "rr"));
}

// §28.3 / §28.9: a deeper nesting — `f` at method scope, `rr` two blocks in. The
// target is still shallower than the source, so the escape is rejected.
BEZEL_TEST(closure_escape_outer_local_861, escape_from_doubly_nested_block_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var f: () -> int := () => -1; "
        "do "
        "  do "
        "    using var rr := new R(7); "
        "    f := () => rr.V; "
        "  end; "
        "end; "
        "return f(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsOuterLocalEscape(r, "rr"));
}

// =============================================================================
// POSITIVE — non-escaping closures stay accepted (no over-rejection).
// =============================================================================

// §28.3: the closure is assigned to a SAME-scope local — it dies with `rr`, so
// it never outlives the capture. Accepted.
BEZEL_TEST(closure_escape_outer_local_861, same_scope_closure_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "using var rr := new R(55); "
        "var f: () -> int := () => rr.V; "
        "return f(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: the closure target `g` is declared in an INNER block, strictly deeper
// than the captured `rr`. `g` dies before `rr`, so it does not outlive the
// capture — not an escape, accepted.
BEZEL_TEST(closure_escape_outer_local_861, inner_block_target_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "using var rr := new R(55); "
        "do "
        "  var g: () -> int := () => rr.V; "
        "  var n := g(); "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: a closure assigned to an enclosing local but capturing NO owned local
// (only a value-typed `int`) is not constrained — value captures carry no
// scope-bound disposal obligation. Accepted.
BEZEL_TEST(closure_escape_outer_local_861, no_owned_capture_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var f: () -> int := () => -1; "
        "  do "
        "    var k := 7; "
        "    f := () => k; "
        "  end; "
        "  return f(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
