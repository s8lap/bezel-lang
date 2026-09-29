// fix #4722 (b) (§22.5 "Optional parameters and named arguments" / §9.3
// "Combined form" / §22.6, v0.307): codegen's constructor selector
// `ctorForArgs` (src/codegen/LlvmBackend_impl.hpp) type-checked a candidate
// by pairing call-site argument i with declared parameter i — positionally —
// even for a NAMED argument. §22.5 binds a named argument to the parameter
// of that name, "in any order after positional arguments", and states that
// the binding is the same for "a call, a `new` expression, and a record
// construction alike". So `new P(n := 4, label := "abc")` against
// `constructor(label: string, n: int)` compared the int `4` with the string
// parameter `label`, rejected the only candidate sema had accepted (#4712
// defers combined-form record `new` selection to codegen), and:
//   * as a `var p := new P(...)` initializer, `lowerLocalDecl`'s #2434
//     fallback silently ZERO-FILLED the record (the program printed `0 0`);
//   * elsewhere the `new` failed to lower ("no constructor of `P` accepts
//     them").
// The same selector serves classes, so the class twin printed `0` too.
//
// The fix binds each argument to its §22.5 parameter slot (positional in
// order, named by name) before the per-argument type-class comparison.
//
// Every row is a VALUE assertion on stdout — a construction that compiles
// but zero-fills is exactly the pre-fix failure mode, so `ran`/exit code
// alone would prove nothing.
//
// Not covered here: #4722 (a) — a combined-form record body constructor's
// `this(...)` chain to the auto-generated primary storing nothing — whose
// fix site is outside this change (see the #4722 handback).
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// The issue's shape, minimised: all-named, reordered, with the two
// parameters of DIFFERENT type classes (string vs int) so the positional
// pairing mismatches. Pre-fix: compile error "no constructor of `P` accepts
// them" (not a local-decl initializer here, so no zero-fill fallback).
BEZEL_TEST(codegen_record_combined_form_named_args_4722,
           all_named_reordered_body_ctor_spec_22_5) {
    auto r = runAot(
        "module M; "
        "public record P(X: int, Y: int) "
        "  public constructor(label: string, n: int) "
        "    X := n * 10 + label.Length; "
        "    Y := 99; "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new P(n := 4, label := \"abc\"); "
        "    Console.WriteLine($\"{p.X} {p.Y}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "43 99\n");
}

// Three parameters, all-named in a rotated order, then MIXED positional +
// named (§22.5: "Named arguments may appear in any order after positional
// arguments"). Pre-fix the first construction silently zero-filled
// (`0 0` + the #2434 internal warning); the mixed one happened to pass
// because its type classes line up positionally — kept as a guard that the
// positional prefix still binds in order.
BEZEL_TEST(codegen_record_combined_form_named_args_4722,
           rotated_and_mixed_named_args_spec_22_5) {
    auto r = runAot(
        "module M; "
        "public record P(X: int, Y: int) "
        "  public constructor(label: string, n: int, bonus: int) "
        "    X := n * 10 + label.Length; "
        "    Y := bonus; "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new P(n := 4, bonus := 5, label := \"abc\"); "
        "    Console.WriteLine($\"{p.X} {p.Y}\"); "
        "    var q := new P(\"hello\", bonus := 6, n := 7); "
        "    Console.WriteLine($\"{q.X} {q.Y}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "43 5\n75 6\n");
}

// A positional STRING field written by the named-arg body constructor,
// constructed in a loop (the stored string is a fresh `+1` each iteration).
// Pre-fix: compile error. Pins the stored string value, not just the int.
BEZEL_TEST(codegen_record_combined_form_named_args_4722,
           named_args_body_ctor_writes_string_field_in_loop) {
    auto r = runAot(
        "module M; "
        "public record P(Name: string, Y: int) "
        "  public constructor(n: int, label: string) "
        "    Name := label + \"!\"; "
        "    Y := n; "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main(): int "
        "    for var i in 0..<50 do "
        "      var p := new P(label := $\"x{i}\", n := i); "
        "      if i = 49 then Console.WriteLine($\"{p.Name} {p.Y}\"); end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "x49! 49\n");
}

// The class twin: `ctorForArgs` is the same selector for a class `new`, and
// the same reordered named call silently zero-filled the object (`0`).
BEZEL_TEST(codegen_record_combined_form_named_args_4722,
           class_twin_named_reordered_spec_22_5) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property X: int; "
        "  public constructor(label: string, n: int) "
        "    X := n * 10 + label.Length; "
        "  end constructor; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var c := new C(n := 4, label := \"abc\"); "
        "    Console.WriteLine(c.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "43\n");
}

// Guards (passed before the fix, must keep passing): same-type-class
// reordered names, and a defaulted tail omitted through named args
// (`ctorForArgs`'s fill pass, `argsCanFill`, already name-aware), plus the
// primary constructor still reached by its own field names.
BEZEL_TEST(codegen_record_combined_form_named_args_4722,
           guards_same_class_reorder_default_tail_and_primary) {
    auto r = runAot(
        "module M; "
        "public record P(X: int, Y: int) "
        "  public constructor(a: int, b: int, c: int := 4) "
        "    X := a * 100 + b * 10 + c; "
        "    Y := a - c; "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new P(c := 3, a := 1, b := 2); "
        "    Console.WriteLine($\"{p.X} {p.Y}\"); "
        "    var q := new P(b := 2, a := 1); "
        "    Console.WriteLine($\"{q.X} {q.Y}\"); "
        "    var r := new P(Y := 7, X := 8); "
        "    Console.WriteLine($\"{r.X} {r.Y}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "123 -2\n124 -3\n8 7\n");
}
