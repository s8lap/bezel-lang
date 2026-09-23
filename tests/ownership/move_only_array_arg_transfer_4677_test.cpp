// Spec-derived regression for issue #4677: a move-only inline array
// (§15.1.1 `[N owned]T?` / `[N owned]T`) passed as a PLAIN (unmarked)
// argument transfers ownership out of the caller's binding, exactly as a
// local-to-local assignment does — but nothing tracked that transfer, so
// the caller could keep reading the "moved-from" array with no diagnostic.
//
// §15.3 "Rules", *Parameter passing*:
//   "a parameter declared `(x: V)` for an ordinary V is by value (copy); for
//    a move-only V, it is by value-move (transfer ownership). The caller's
//    argument expression is in moved-from state after the call."
// §15.3 "Rules", *Local-to-local assignment* (the sibling rule this mirrors):
//   "moves for a move-only value type. After the move, `x` is in moved-from
//    state and may not be read until reassigned."
//
// Root cause: a move-only-typed formal is NEVER `ownedSink`/`isImplicitOwned`
// — §28.1 clause 1 makes its `owned` marker admitted-but-redundant, so the
// existing "explicit `owned` formal" transfer-marking never applied to it —
// and the plain bare-identifier argument fallthrough (`recordMovesIn`) has
// no IdentifierExpr arm at all. This is exactly the local-to-local-move gap
// #1899/#1922 already closed for `var b := a;` and a plain-parameter source,
// via `noteMoveOnlyArrayWholeMoveRead` — but that primitive was wired into
// only ONE of FOUR call-argument-recording sites in Ownership_Checks.cpp
// (the codebase's own "Family I1" duplicated-recorder shape): `recordMovesIn`'s
// CallExpr arm (`recordMovesInArgs`) and NewExpr arm cover a call/`new` used
// as a LocalDecl initializer or assignment RHS; `handleDischarge`'s CallExpr
// and NewExpr arms cover the identical shapes as a BARE STATEMENT. All four
// are exercised below. Mirrors the rule-corpus case s15_3_0010
// (tests/conformance/rules/cases) for the bare-statement CallExpr shape.
//
// The corpus's own baseline notes a diagnostic-WORDING mismatch (the fired
// message says "was already moved out at this point (§28.3)", not the
// literal word "moved-from") — the same pre-existing wording `move_only_
// array_param_move_1922_test.cpp`'s `hasMovedOutError` helper already keys
// on, reused here.

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

const char* kWidget =
    "module M; "
    "class Widget "
    "  public Tag: string; "
    "  public constructor(tag: string) Tag := tag; end constructor; "
    "end Widget; ";

// True iff some ERROR diagnostic reports a §28.3 moved-out use of `name` —
// mirrors move_only_array_param_move_1922_test.cpp's identical helper.
bool hasMovedOutError(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("moved out") == std::string::npos) continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// Site 1: handleDischarge's CallExpr arm — a BARE-STATEMENT method call. The
// rule-corpus repro (s15_3_0010) shape exactly.
// =============================================================================
BEZEL_TEST(move_only_array_arg_transfer_4677,
           bare_statement_call_arg_moved_from_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(std::string(kWidget) +
        "class Sink "
        "  public static method Take(a: [1 owned]Widget) "
        "    Console.WriteLine(a[0].Tag); "
        "  end Take; "
        "end Sink; "
        "class Program "
        "  public static method Run() "
        "    var a: [1 owned]Widget := [new Widget(\"x\")]; "
        "    Sink.Take(a); "
        "    Console.WriteLine(a[0].Tag); "
        "  end Run; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// The same shape at RUNTIME: the pre-fix program was accepted and ran without
// crashing (memory-safe — the callee's transfer never frees anything the
// caller reads), which is exactly why this needed a static rule rather than
// relying on a sanitizer to catch it. Confirms the fix does not merely
// reject valid code: dropping the SECOND read compiles and runs clean.
BEZEL_TEST(move_only_array_arg_transfer_4677,
           bare_statement_call_single_use_accepted_and_runs) {
    auto r = runAot(std::string(kWidget) +
        "class Sink "
        "  public static method Take(a: [1 owned]Widget) "
        "    Console.WriteLine(a[0].Tag); "
        "  end Take; "
        "end Sink; "
        "class Program "
        "  public static method Main() "
        "    var a: [1 owned]Widget := [new Widget(\"x\")]; "
        "    Sink.Take(a); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =============================================================================
// Site 2: recordMovesInArgs (via recordMovesIn's CallExpr arm) — a method
// call used as a LocalDecl INITIALIZER.
// =============================================================================
BEZEL_TEST(move_only_array_arg_transfer_4677,
           localdecl_init_call_arg_moved_from_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(std::string(kWidget) +
        "class Sink "
        "  public static method TagOf(a: [1 owned]Widget): string "
        "    return a[0].Tag; "
        "  end TagOf; "
        "end Sink; "
        "class Program "
        "  public static method Run() "
        "    var a: [1 owned]Widget := [new Widget(\"x\")]; "
        "    var t: string := Sink.TagOf(a); "
        "    Console.WriteLine(a[0].Tag); "
        "  end Run; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// =============================================================================
// Site 3: handleDischarge's NewExpr arm — a `new T(a)` constructor call used
// as a BARE STATEMENT.
// =============================================================================
BEZEL_TEST(move_only_array_arg_transfer_4677,
           bare_statement_ctor_arg_moved_from_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(std::string(kWidget) +
        "class Holder "
        "  private _a: [1 owned]Widget; "
        "  public constructor(a: [1 owned]Widget) _a := a; end constructor; "
        "end Holder; "
        "class Program "
        "  public static method Run() "
        "    var a: [1 owned]Widget := [new Widget(\"x\")]; "
        "    new Holder(a); "
        "    Console.WriteLine(a[0].Tag); "
        "  end Run; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// =============================================================================
// Site 4: recordMovesIn's NewExpr arm — a `new T(a)` constructor call used as
// a LocalDecl INITIALIZER.
// =============================================================================
BEZEL_TEST(move_only_array_arg_transfer_4677,
           localdecl_init_ctor_arg_moved_from_rejected_spec_15_3) {
    auto r = parseAndCheckOwnership(std::string(kWidget) +
        "class Holder "
        "  private _a: [1 owned]Widget; "
        "  public constructor(a: [1 owned]Widget) _a := a; end constructor; "
        "end Holder; "
        "class Program "
        "  public static method Run() "
        "    var a: [1 owned]Widget := [new Widget(\"x\")]; "
        "    using var h := new Holder(a); "
        "    Console.WriteLine(a[0].Tag); "
        "  end Run; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, "a"));
}

// =============================================================================
// Negative control: an ORDINARY class-typed (non-move-only) argument must NOT
// be affected by this fix — a plain borrow parameter still leaves the caller's
// binding fully readable after the call.
// =============================================================================
BEZEL_TEST(move_only_array_arg_transfer_4677,
           ordinary_class_typed_borrow_arg_unaffected) {
    auto r = parseAndCheckOwnership(std::string(kWidget) +
        "class Sink "
        "  public static method Peek(w: Widget) "
        "    Console.WriteLine(w.Tag); "
        "  end Peek; "
        "end Sink; "
        "class Program "
        "  public static method Run() "
        "    using var w := new Widget(\"x\"); "
        "    Sink.Peek(w); "
        "    Console.WriteLine(w.Tag); "
        "  end Run; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
