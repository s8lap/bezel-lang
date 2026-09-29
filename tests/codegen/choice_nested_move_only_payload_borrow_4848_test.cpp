// Regression tests for #4848 (§19.3/§35.5, v0.308): a nested destructure of a
// MOVE-ONLY (owning) choice payload inside a borrowing match — the case
// `bindNestedDestructuringNames` (src/sema/ExprTypecheck.cpp) never handled a
// NESTED pattern whose head names a CHOICE CASE rather than a record. Its
// `fieldsOfRecord` helper only resolves a top-level `TypeSymbol` of kind
// `Record` by name, so `Wrap(W(var a, var b))` — where `Wrap`'s payload
// `inner: Outer` is itself an OWNING choice (`Outer` has `owned` Widget
// fields) and `W` is one of `Outer`'s own cases — left `ndFields` empty:
// every binding beneath the nested pattern (`a`, `b`) got a null `declType`,
// so the arm body's read through it (`a.Ping()`) silently failed to lower to
// a value (codegen's #2961 internal Warning, "store dropped") instead of
// compiling to the borrow §35.5 "The borrowing match" specifies:
//
//   "a binding of a payload of move-only value type (an owning inline array,
//   or a nested owning choice) is likewise a borrow … reads through the
//   binding are admitted."
//
// The fix threads the parent slot's substituted scrutinee TypeRef through
// `findChoiceCaseOnScrutinee` + `substitutedChoicePayloadType` (the same
// choice-side lookup the four TOP-LEVEL caller loops already use) before
// falling back to the record-only `fieldsOfRecord` lookup.
//
// FAIL-WITHOUT (mechanically proven): reverting the fix reproduces the
// issue's own literal repro — the statement-form arm silently drops the
// store (`m=0` instead of `m=7`), and the expression-form arm degrades to
// "Basic Block ... does not have terminator!" IR-verification failure
// (`r.ran == false`). Both rows below pin the CORRECTED behavior.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Shared declarations: a destructor-bearing class payload nested two levels
// deep through an OWNING choice (Outer) inside another choice (Box).
const char kBoxOuterWidgetDecl[] =
    "public class Widget "
    "  public constructor() end constructor; "
    "  destructor Console.Write(\"\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public choice Box Wrap(inner: Outer), Nothing; end Box;";

}  // namespace

// --- §35.5 / §19.3: the issue's own statement-form repro --------------------

// Issue #4848 MEASURED shape (statement-position arm body `do m := a.Ping();
// end`). Before the fix this compiled with only a Warning and printed
// "m=0" — the arm IS selected (the other arm would print 98) but the store
// through `a.Ping()` silently dropped. §35.5's borrowing-match rule admits
// the read, so `m` must be 7.
BEZEL_TEST(codegen_choice_nested_move_only_borrow_4848,
           spec_35_5_statement_form_nested_owning_choice_bind_reads) {
    auto r = runAot(
        std::string("module M; ") + kBoxOuterWidgetDecl +
        " public class Program "
        "  public static method Main(): int "
        "    var bx := Box.Wrap(Outer.W(new Widget(), new Widget())); "
        "    var m := 0; "
        "    case bx of "
        "      Wrap(W(var a, var b)) => do m := a.Ping(); end, "
        "      _ => do m := 98; end "
        "    end case; "
        "    Console.WriteLine($\"m={m}\"); "
        "    return m; "
        "  end Main; "
        " end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
    BEZEL_EXPECT_EQ(r.stdoutText, "m=7\n");
}

// The second binding (`b`) must resolve identically — proves the fix is not
// an off-by-one that only happens to fix slot 0.
BEZEL_TEST(codegen_choice_nested_move_only_borrow_4848,
           spec_35_5_statement_form_second_nested_slot_binds_too) {
    auto r = runAot(
        std::string("module M; ") + kBoxOuterWidgetDecl +
        " public class Program "
        "  public static method Main(): int "
        "    var bx := Box.Wrap(Outer.W(new Widget(), new Widget())); "
        "    var m := 0; "
        "    case bx of "
        "      Wrap(W(var a, var b)) => do m := b.Ping(); end, "
        "      _ => do m := 98; end "
        "    end case; "
        "    return m; "
        "  end Main; "
        " end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// --- §35.5 / §19.3: the issue's own expression-form repro -------------------

// Issue #4848's expression-position twin: before the fix this failed IR
// verification ("Basic Block in function 'main' does not have terminator!"),
// so `runAot` reports `ran == false`. With the fix it must compile and run,
// returning 7.
BEZEL_TEST(codegen_choice_nested_move_only_borrow_4848,
           spec_35_5_expression_form_nested_owning_choice_bind_reads) {
    auto r = runAot(
        std::string("module M; ") + kBoxOuterWidgetDecl +
        " public class Program "
        "  public static method Main(): int "
        "    var bx := Box.Wrap(Outer.W(new Widget(), new Widget())); "
        "    var n := (case bx of Wrap(W(var a, var b)) => a.Ping(), _ => 99); "
        "    return n; "
        "  end Main; "
        " end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// --- Control: the no-match arm must still be selected correctly ------------

// A `bx` built from the OTHER outer case (`X`, payload-free) must fall
// through to `_`, proving the fix did not turn the nested pattern into an
// unconditional match.
BEZEL_TEST(codegen_choice_nested_move_only_borrow_4848,
           spec_35_5_control_no_match_falls_through) {
    auto r = runAot(
        std::string("module M; ") + kBoxOuterWidgetDecl +
        " public class Program "
        "  public static method Main(): int "
        "    var bx := Box.Nothing; "
        "    var m := 0; "
        "    case bx of "
        "      Wrap(W(var a, var b)) => do m := a.Ping(); end, "
        "      _ => do m := 98; end "
        "    end case; "
        "    return m; "
        "  end Main; "
        " end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 98);
}

// --- Leak/double-free sensitivity: loop 200x ---------------------------------

// The nested binding reads through class-typed owned payloads reached two
// choice levels deep. Run the whole construct-match-dispose cycle 200x in one
// process so a double-free or leak on the destructor-bearing Widget payloads
// shows up as a crash or (under ASan, run separately with
// BEZEL_AOT_FORCE_SANITIZE=address) a heap-use-after-free/leak report, not a
// one-shot false negative.
BEZEL_TEST(codegen_choice_nested_move_only_borrow_4848,
           spec_35_5_loop_200x_no_leak_or_double_free) {
    // #4848 lane rule: never assert an exit code above 255 (8-bit
    // truncation) — 200 * (7 + 7) = 2800 does not fit, so the total is
    // printed to stdout and checked there instead, with exit code 0.
    auto r = runAot(
        std::string("module M; ") + kBoxOuterWidgetDecl +
        " public class Program "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var bx := Box.Wrap(Outer.W(new Widget(), new Widget())); "
        "      var m := 0; "
        "      case bx of "
        "        Wrap(W(var a, var b)) => do m := a.Ping() + b.Ping(); end, "
        "        _ => do m := 0; end "
        "      end case; "
        "      total := total + m; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        " end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "total=2800\n");  // 200 * (7 + 7)
}
