// Spec-derived tests for Ownership slice O14 — call arguments that
// consume ownership (§22.3 argument modifiers, §28.3 move).
//
// `M(owned x)` and `M(move x)` both transfer x's ownership to the
// callee; afterwards x is in MovedFrom state and any read of x is a
// diagnostic. The undischarged-owned-local check at scope exit treats
// the move as a discharge, so a plain `var x := new T(); M(owned x);`
// is clean.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    // Sink.Take's `owned x: T` parameter is itself an owned obligation;
    // we discharge it with `using var hold := move x;` so the wrapper
    // doesn't surface unrelated diagnostics.
    return std::string("module M; "
        "class T end T; "
        "class Sink "
        "  public method Take(owned x: T) "
        "    using var hold := move x; "
        "  end Take; "
        "end Sink; ") + body;
}

} // namespace

BEZEL_TEST(own_call_arg, owned_arg_modifier_consumes_local) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    var x := new T(); "
        "    s.Take(owned x); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_call_arg, move_arg_consumes_local) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    var x := new T(); "
        "    s.Take(move x); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_call_arg, read_after_owned_arg_diagnoses) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    var x := new T(); "
        "    s.Take(owned x); "
        "    _ := x; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_call_arg, read_after_move_arg_diagnoses) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    var x := new T(); "
        "    s.Take(move x); "
        "    _ := x; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_call_arg, owned_arg_in_initializer_consumes) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method Pass(owned x: T): owned T => move x; "
        "  public method M() "
        "    var x := new T(); "
        "    using var y := this.Pass(owned x); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_call_arg, plain_arg_does_not_consume) {
    auto r = parseAndCheckOwnership(mod(
        "class Inspector public method Peek(x: T) _ := 1; end Peek; end Inspector; "
        "class C "
        "  public method M(i: Inspector) "
        "    using var x := new T(); "
        "    i.Peek(x); "
        "    _ := x; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
