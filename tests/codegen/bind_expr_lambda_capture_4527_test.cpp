// Codegen — a §17.15 binding atom `(var y := E; body)` inside a lambda body
// must see the lambda's captures (§17.15 / §27.5, #4527).
//
// Spec §17.15 (v0.216): "the binding atom `(var x := E; body)` — a
// PrimaryAtom ... Clauses are represented as embedded plain-`var` LocalDecl
// nodes ... the body is one full Expression supplying the atom's value, type,
// and ownership classification". Spec §27.5: "A lambda may reference
// variables from its enclosing scope. Captured variables are heap-allocated
// in a closure object."
//
// THE DEFECT. `CaptureWalker::expr` (LlvmBackend_Layout.cpp), the walker that
// decides which enclosing symbols a lambda captures — and therefore which env
// fields it gets — had no `ast::BindExpr` arm. A `BindExpr` fell through to
// the "literals and other leaf expressions: nothing to do" tail, so BOTH its
// clause initializers and its body expression were invisible. An enclosing
// local referenced ONLY inside a binding atom therefore never reached
// `capturedSyms`, the closure got no env field for it, and the reference did
// not lower. Measured on origin/main (6c05b4cf), `bezelc`:
//
//     var x := 7;
//     var f := lambda(): int => (var y := x; y + 1);
//
//     internal: lowerLocalDecl's initializer expression failed to lower to a
//     value for this local's slot (degraded to a zero-initialized value) —
//     #2434
//
// — i.e. an over-rejection of valid §17.15 code, not a memory defect. The
// issue filed it as hygiene ("the next copy of the walker whose gap caused
// #4523") on the ground that a decl invisible to `capturedSyms` gets no env
// field and so cannot become an uncounted cell holder. That reasoning is
// correct about the #4523 use-after-free class and does not reach this:
// getting no env field is exactly what breaks the read.
//
// TWO COPIES OF THE WALK, both blind, and fixing one alone is not enough.
// `CaptureWalker` answers "which enclosing symbols does this lambda capture?"
// (env layout); `LambdaFinder::walkExpr` inside `collectCellFreeEligible`
// answers "does any lambda capture this local?" (whether the cell may be
// freed at scope end). With only the first arm added the program compiles and
// computes the right value while leaking its capture cell — 2000 B / 500
// allocations at x500, measured. Row 6 is that half. This is the "same
// question answered in more than one place" the issue names as the family's
// defect generator; collapsing the copies is worth doing and is not this fix.
//
// BOUNDARY, measured, so the diagnosis is not "BindExpr in a lambda is
// broken": a binding atom inside a lambda that captures NOTHING compiles and
// runs (row 4), and one over the lambda's OWN PARAMETER does too (row 5) —
// parameters are bound by the lambda, not captured through the walker. Only
// the capture path was blind, which is what rows 1-3 pin.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

namespace {

int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ============================================================================
// Row 1 (#4527's repro, EXPRESSION-BODIED lambda): the binding atom IS the
// lambda's whole body, and its clause initializer is the only reference to
// the enclosing `x`.
//
// Pre-fix: the `internal:` diagnostic above — `runAotChecked` reports
// ran=false (-999). Post-fix: 7 + 1 == 8.
// ============================================================================
BEZEL_TEST(codegen_bind_expr_lambda_capture_4527,
           expression_body_binding_atom_captures_outer_local) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x := 7; "
        "    var f := lambda(): int => (var y := x; y + 1); "
        "    return f(); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 8);
}

// ============================================================================
// Row 2 (BLOCK-BODIED lambda): the same atom at a `var z :=` initializer
// position inside a `do … end` body, so the walk reaches it through
// `stmt`'s LocalDecl arm rather than through `lam->exprBody`. Pre-fix: the
// same `internal:` diagnostic at the atom's own column. Post-fix: 8.
// ============================================================================
BEZEL_TEST(codegen_bind_expr_lambda_capture_4527,
           block_body_binding_atom_captures_outer_local) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x := 7; "
        "    var f := lambda(): int => do "
        "      var z: int := (var y := x; y + 1); "
        "      return z; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 8);
}

// ============================================================================
// Row 3 (THE CAPTURE IS REAL, NOT A STACK READ): the closure ESCAPES its
// defining frame and is called from `Main` strictly after `Make` has
// returned. A "fix" that let the lambda read the enclosing frame's alloca
// directly would pass rows 1-2 (the frame is still live there) and read
// freed stack here — under `runAotChecked` that is a sanitizer abort, not a
// wrong number. Post-fix: 8.
// ============================================================================
BEZEL_TEST(codegen_bind_expr_lambda_capture_4527,
           escaping_closure_binding_atom_reads_capture_cell) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Make(): () -> int "
        "    var x := 7; "
        "    var f := lambda(): int => (var y := x; y + 1); "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var g := C.Make(); "
        "    return g(); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 8);
}

// ============================================================================
// Row 4 (BOUNDARY — no capture): a binding atom inside a lambda that
// references nothing from an enclosing scope. Measured working on
// origin/main (6c05b4cf) and must stay working: it is the control that says
// the defect is in the CAPTURE walk, not in lowering a binding atom inside a
// lambda at all.
// ============================================================================
BEZEL_TEST(codegen_bind_expr_lambda_capture_4527,
           binding_atom_without_capture_still_lowers) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var z: int := (var y := 3; y + 1); "
        "      return z; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 4);
}

// ============================================================================
// Row 5 (BOUNDARY — the lambda's OWN parameter): a binding atom over a
// parameter, which the lambda binds rather than captures, so it never went
// through `CaptureWalker` at all. Measured working on origin/main and
// pinned here for the same reason as row 4.
// ============================================================================
BEZEL_TEST(codegen_bind_expr_lambda_capture_4527,
           binding_atom_over_lambda_parameter_still_lowers) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(p: int): int => (var y := p; y + 1); "
        "    return f(3); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 4);
}

// ============================================================================
// Row 6 (LEAK-AMPLIFIED x500 — THE SECOND COPY OF THE WALK): admitting the
// binding atom into `CaptureWalker` alone is only half the fix, and this row
// is what says so. `CaptureWalker` decides that the outer local IS captured,
// so `boxLocalSlot` allocates its cell; `LambdaFinder::walkExpr` inside
// `collectCellFreeEligible` decides whether any lambda captures it at all,
// and with no arm there the answer stayed "none" — `cellFreeEligible_` never
// armed and the cell leaked, exactly as #3591 measured for the sibling
// `LocalConstDecl` gap in the same walk.
//
// Measured, three compilers, `bezelc --sanitize=address`:
//   origin/main 6c05b4cf          — does not compile (the `internal:` above)
//   + CaptureWalker arm only      — 4000 printed, 2000 B / 500 allocations
//   + both arms                   — 4000 printed, clean
//
// A one-shot row would pass vacuously here (one 4-byte cell, reachable from a
// live stack slot), which is why this is the amplified one. The value is
// re-asserted inside the program so exit 0 cannot be reached by a run that
// merely happens not to leak.
// ============================================================================
BEZEL_TEST(codegen_bind_expr_lambda_capture_4527,
           binding_atom_capture_cell_reclaimed_leak_amplified_500x) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static method Once(): int "
        "    var x := 7; "
        "    var f := lambda(): int => (var y := x; y + 1); "
        "    return f(); "
        "  end Once; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      total := total + C.Once(); "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    if total <> 4000 then "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 0);
}
