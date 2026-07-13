// Codegen: checked disposal mode (issue #601 / §28.2 v0.134 / §31.4).
//
// §28.2 "Use after disposal; checked disposal mode" specifies two conformance
// modes for the per-build disposal-checking configuration:
//
//   Unchecked (default): use of a disposed instance is undefined behavior —
//   ZERO change to the existing codegen baseline.
//
//   Checked (--checked-disposal): any instance member access on a logically-
//   disposed instance raises DisposedAccessException (§31.4). A further
//   Dispose() call remains a no-op, not an error, in both modes. Correct
//   programs (no use-after-dispose) behave identically in both modes.
//
// §31.4 places DisposedAccessException under InvalidOperationException in the
// built-in exception hierarchy, so a `catch var e: InvalidOperationException`
// arm catches it.
//
// Tests derived from §28.2 examples and invariants (v0.134):
//   1. Instance method call on disposed instance → DisposedAccessException.
//   2. Property accessor on disposed instance → DisposedAccessException.
//   3. Second Dispose() on disposed instance → no-op (no throw).
//   4. DisposedAccessException is-a InvalidOperationException (hierarchy §31.4).
//   5. A correct program (no use-after-dispose) produces identical output in
//      both modes (default vs. checked).
//   6. Instance method call on a NOT-yet-disposed instance succeeds normally
//      in checked mode.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {

// Convenience wrapper: run in checked-disposal MODE (§28.2 — DisposedAccess-
// Exception on use-after-dispose), via the AOT harness; returns exit code, or
// -999 if the program failed to compile/run. NB checked-disposal MODE, not the
// ASan memory net (that is runAotChecked) — see support/aot_support.hpp.
int runChecked(const std::string& program) {
    auto r = runAotCheckedDisposal(program);
    return r.ran ? r.exitCode : -999;
}

// Convenience wrapper: run in default (unchecked) mode.
int runDefault(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace


// ── §28.2 / §31.4 v0.134: instance method call on disposed instance ──────

BEZEL_TEST(codegen_checked_disposal_601, spec_28_2_method_call_raises) {
    // §28.2 v0.134: calling an instance method on a disposed instance in
    // checked mode raises DisposedAccessException.  The try arm catches it
    // and returns 1 to signal the raise happened.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public method GetValue(): int return 42; end GetValue; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    obj.Dispose(); "
        "    try "
        "      var v := obj.GetValue(); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}


// ── §28.2 / §31.4 v0.134: property accessor on disposed instance ─────────

BEZEL_TEST(codegen_checked_disposal_601, spec_28_2_property_read_raises) {
    // §28.2 v0.134: reading a property on a disposed instance raises
    // DisposedAccessException in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public property Value: int => 7; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    obj.Dispose(); "
        "    try "
        "      var v := obj.Value; "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}


// ── §28.2: second Dispose() is a no-op in both modes ─────────────────────

BEZEL_TEST(codegen_checked_disposal_601, spec_28_2_second_dispose_noop_checked) {
    // §28.2 v0.134: "A further Dispose() call remains a no-op, not an error,
    // in both modes." In checked mode, calling Dispose() a second time must
    // NOT raise DisposedAccessException — it silently returns.
    // The destructor increments _n on first disposal only (idempotency is
    // tested here indirectly: if a second Dispose raised we would not reach
    // `return _n`).
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    obj.Dispose(); "
        "    obj.Dispose(); "
        "    return _n; "
        "  end Main; "
        "end C;"), 1);
}


// ── §31.4: DisposedAccessException is-a InvalidOperationException ─────────

BEZEL_TEST(codegen_checked_disposal_601, spec_31_4_hierarchy_caught_by_base) {
    // §31.4 v0.134: DisposedAccessException is under InvalidOperationException
    // in the hierarchy. A `catch var e: InvalidOperationException` arm must
    // catch a DisposedAccessException.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public method GetValue(): int return 99; end GetValue; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    obj.Dispose(); "
        "    try "
        "      var v := obj.GetValue(); "
        "      return 0; "
        "    catch var e: InvalidOperationException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}


// ── §28.2: correct program (no use-after-dispose): identical output ───────

BEZEL_TEST(codegen_checked_disposal_601, spec_28_2_correct_program_default_mode) {
    // §28.2 v0.134: "The two modes give identical observable behavior for
    // every program that never uses a disposed instance." Verify the correct-
    // program output in DEFAULT (unchecked) mode.
    const std::string prog =
        "module M; class C "
        "  public method GetValue(): int return 42; end GetValue; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    var v := obj.GetValue(); "
        "    return v; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runDefault(prog), 42);
}

BEZEL_TEST(codegen_checked_disposal_601, spec_28_2_correct_program_checked_mode) {
    // §28.2 v0.134: same correct program in CHECKED mode must produce the
    // same exit code (42).
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public method GetValue(): int return 42; end GetValue; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    var v := obj.GetValue(); "
        "    return v; "
        "  end Main; "
        "end C;"), 42);
}


// ── §28.2: not-yet-disposed instance still accessible in checked mode ─────

BEZEL_TEST(codegen_checked_disposal_601, spec_28_2_live_instance_accessible) {
    // §28.2 v0.134: the guard fires only for a DISPOSED instance. Accessing a
    // live instance must succeed normally in checked mode without any exception.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public method Add(x: int, y: int): int return x + y; end Add; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    return obj.Add(3, 4); "
        "  end Main; "
        "end C;"), 7);
}
