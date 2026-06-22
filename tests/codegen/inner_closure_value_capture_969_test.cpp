// Codegen — issue #969: an INNER-scope closure capturing an outer VALUE local
// must not free the capture cell at the inner block's exit (§27.5 / §28.9).
//
// The value-typed companion to #966. `do var g := () => x; end` boxes the outer
// `var x` into a heap cell shared with the closure; the non-escaping closure's
// scope-end `boxedCellFree` freed that cell at the INNER block's exit, but
// `return x` (outer scope) reads `x` THROUGH the cell → a corrupted value (the
// freed cell's bytes), e.g. 67 instead of 55. #966's fix only covered OWNED
// locals (whose using-var disposal is a reads-through-cell disposal the guard
// inspects); a VALUE local has no such disposal, so the guard missed it.
//
// THE FIX: record each local's block-nesting depth (declScopeDepth_) and skip
// `boxedCellFree` at the inner lambda's scope when the captured local was
// declared in an ENCLOSING scope — the cell then leaks until the local's own
// scope exits (the sound trade-off).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// The repro: capture an outer VALUE local in an inner-scope closure, then read
// the outer local after the inner block → must still be 55 (was garbage).
BEZEL_TEST(inner_closure_value_capture_969, value_local_intact_after_inner_block) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 55; "
        "  do var g: () -> int := () => x; var k := g(); end; "
        "  return x; "  // 55
        "end Main; end C;"), 55);
}

// Two levels of nesting between the captured value local and the closure.
BEZEL_TEST(inner_closure_value_capture_969, doubly_nested_value_capture) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 40; "
        "  do do var g: () -> int := () => x; var k := g(); end; end; "
        "  return x + 2; "  // 42
        "end Main; end C;"), 42);
}

// The captured value is also correct INSIDE the inner block (g() == x) and the
// outer read after the block is unaffected.
BEZEL_TEST(inner_closure_value_capture_969, captured_value_correct_inside_and_after) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 33; "
        "  var sum := 0; "
        "  do var g: () -> int := () => x; sum := g(); end; "
        "  return x + sum; "  // 66
        "end Main; end C;"), 66);
}

// Regression: a SAME-scope value capture is unaffected (the cell is freed at the
// same scope, after the read).
BEZEL_TEST(inner_closure_value_capture_969, same_scope_value_capture_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 21; "
        "  var f: () -> int := () => x; "
        "  return f() + x; "  // 42
        "end Main; end C;"), 42);
}
