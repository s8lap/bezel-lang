// #7199 (spec §22.5, stdlib §2.2 / §2.3, v0.343) — the `Math` / `Convert`
// STATIC member arms must bind a NAMED argument to the PARAMETER it names.
//
// WHAT IS BROKEN. §22.5 (v0.343) says "named arguments may appear in any
// order after positional arguments. Each supplied argument binds exactly one
// parameter", and "naming an argument changes which parameter it binds to,
// never when it runs". Sema binds a built-in member's named arguments
// correctly and PUBLISHES that binding (the `paramToArg` vector: PARAMETER
// index -> SOURCE argument index; see `ExprTC::checkBuiltinNamedArgs` ->
// `SemaContext::setBuiltinCallBinding`). The `Math` / `Convert` static arms
// in `src/codegen/LlvmBackend_Calls.cpp` did not read it: they took their
// operands from `call.args[0]` / `call.args[1]` / `call.args[2]` — the
// POSITIONS — so a reordered named call handed each parameter the value of a
// DIFFERENT parameter.
//
// MEASURED on the base arm (5c7ff044), on a freshly rebuilt `bezelc`:
//
//   Math.Pow(y := 2, x := 3)                   -> 8      (must be 9)
//   Math.Fmod(y := 4, x := 10)                 -> 4      (must be 2)
//   Math.Clamp(high := 10, value := 100, low := 0) -> 0  (must be 10)
//   Convert.ToString(toBase := 16, value := 255)   -> "16" (must be "ff")
//   Math.Round(digits := 3, x := 2.5)          -> 3.001001499499792 (must be 2.5)
//   Math.IEEERemainder(y := 4, x := 10)        -> 4      (must be 2)
//
// EVERY ONE OF THEM COMPILED SILENTLY (compile rc=0, run rc=0, no diagnostic)
// — a silently WRONG ANSWER, the worst outcome class. The DECLARATION-ORDER
// spelling of the very same call was already correct, so the two spellings of
// one call disagreed.
//
// THE FIX. The arms now read the source argument index of each PARAMETER
// from the published §22.5 binding (`ctx_.builtinCallBinding(call)`), and
// lower the supplied argument expressions in CALL-SITE TEXTUAL ORDER (§22.5:
// "A call's supplied argument expressions are evaluated in call-site textual
// order — left to right as written, for positional and named arguments
// alike; naming an argument changes which parameter it binds to, never when
// it runs"). With no binding (a positional call, or a named call already in
// declaration order — the identity permutation, which sema publishes
// NOTHING for) the positional read stands, byte for byte.
//
// THE CONTROLS. Every member also has a control row: the POSITIONAL spelling
// and the named spelling already in DECLARATION order. Those rows must be
// byte-identical before and after the fix: the fix changes the REORDERED
// binding and nothing else.
//
// THE VALUE, NOT THE EXIT CODE. Every row here asserts the program's STDOUT
// (the VALUE channel). A process exit status is masked to 0..255 and cannot
// carry a string value, so an exit-code assertion could not tell "ff" from
// "16", nor 9 from 8.
//
// A NOTE ON THE `Math.Min` / `Math.Max` ROWS. `Min(a, b)` and `Max(a, b)`
// are COMMUTATIVE in value (stdlib §2.2: `Min(a, b)` returns `a` when
// `a.CompareTo(b) <= 0` and `b` otherwise), so the reordered spelling
// yields the same NUMBER as the declaration-order one even when the binding
// is wrong. Their rows are therefore CONTROLS (the reordered spelling must be
// ADMITTED and must agree), not regression rows — no value distinguishes
// them, and the row says so rather than pretending otherwise.
//
// A NOTE ON THE `string` MEMBERS. The `string` instance arms (#7180) and the
// `string.Join` separator gate (#7191) are the SAME class of defect, fixed
// elsewhere. The row below is the cross-check: both of their spellings must
// keep working after this change.
//
// A NOTE ON THE WRONGLY-TYPED NAMED ARGUMENT. stdlib §2.2 declares
// `Pow(x: double, y: double): double`, so a `string` in either slot is not
// implicitly convertible (§5.1). MEASURED on the base arm, the compiler does
// NOT report a §5.1 diagnostic for a `Math` / `Convert` built-in argument at
// all: the wrongly-typed operand reaches codegen and the compiler ABORTS
// with "IR verification failed" — the POSITIONAL spelling does exactly the
// same, so the two spellings agree. That missing per-argument type check is
// a SEPARATE defect (it is not a named-argument defect: the positional
// spelling fails identically) and is NOT fixed here. The row below pins what
// IS true today — the wrongly-typed named argument is REFUSED, exactly as the
// positional spelling is refused — so that this fix cannot silently ADMIT a
// bad program. The row asserts the refusal, not the crash's text, so a
// future proper §5.1 rejection keeps it green.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/emit_object_support.hpp"
#include "support/force_sanitize.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// The programs. Every one of them prints its values, so the row's
// expectation is the printed text.
// ---------------------------------------------------------------------------

// stdlib §2.2: Pow(x: double, y: double): double — `x` is the BASE and `y`
// the EXPONENT, so Pow(3, 2) is 3^2 = 9 and Pow(2, 3) is 2^3 = 8. The
// reordered spelling names the same two parameters and supplies the same two
// arguments as the positional one, so it must produce the same NUMBER.
const char* kPowReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Pow(y := 2, x := 3).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The control: the POSITIONAL spelling and the named spelling already in
// DECLARATION order (the identity permutation — sema publishes nothing for
// it, and the arm reads positionally).
const char* kPowControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Pow(3, 2).ToString()); "
    "    Console.WriteLine(Math.Pow(x := 3, y := 2).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// stdlib §2.2: Fmod(x: double, y: double): double — fmod(10, 4) is 2.
const char* kFmodReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Fmod(y := 4, x := 10).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kFmodControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Fmod(10, 4).ToString()); "
    "    Console.WriteLine(Math.Fmod(x := 10, y := 4).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// stdlib §2.2: IEEERemainder(x: double, y: double): double — the IEEE 754
// remainder of 10 by 4 is 2.
const char* kIeeeReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.IEEERemainder(y := 4, x := 10).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kIeeeControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.IEEERemainder(10, 4).ToString()); "
    "    Console.WriteLine(Math.IEEERemainder(x := 10, y := 4).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// stdlib §2.2: Clamp<T>(value: T, low: T, high: T): T — Clamp(100, 0, 10)
// is 10.
const char* kClampReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Clamp(high := 10, value := 100, low := 0).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kClampControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Clamp(100, 0, 10).ToString()); "
    "    Console.WriteLine(Math.Clamp(value := 100, low := 0, high := 10).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// stdlib §2.3: Convert.ToString(value: int, toBase: int): string —
// ToString(255, 16) is "ff".
const char* kToStringReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Convert.ToString(toBase := 16, value := 255)); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kToStringControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Convert.ToString(255, 16)); "
    "    Console.WriteLine(Convert.ToString(value := 255, toBase := 16)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// stdlib §2.2: Round(x: double, digits: int := 0): double — banker's
// rounding to `digits` FRACTIONAL digits, so Round(2.5, 3) is 2.5.
const char* kRoundReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Round(digits := 3, x := 2.5).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kRoundControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Round(2.5, 3).ToString()); "
    "    Console.WriteLine(Math.Round(x := 2.5, digits := 3).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// stdlib §2.2: Min<T>(a: T, b: T): T and Max<T>(a: T, b: T): T. The
// reordered spelling must be ADMITTED and must agree with the others —
// the two members are commutative in VALUE, so this is a control (see the
// note at the head of this file), not a value-discriminating regression row.
const char* kMinMaxReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Min(b := 2, a := 1).ToString()); "
    "    Console.WriteLine(Math.Max(b := 2, a := 1).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kMinMaxControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Min(1, 2).ToString()); "
    "    Console.WriteLine(Math.Min(a := 1, b := 2).ToString()); "
    "    Console.WriteLine(Math.Max(1, 2).ToString()); "
    "    Console.WriteLine(Math.Max(a := 1, b := 2).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// THE CALL-SITE TEXTUAL ORDER HALF (§22.5). The two argument expressions
// have a side effect (a printed tag) and RETURN CONSTANTS, so the printed
// VALUE does not depend on HOW MANY times the expression is evaluated — only
// the ORDER of the evaluations is observable.
//
// `One()` prints "one" and returns 1; `Two()` prints "two" and returns 2.
// The `y :=` argument is written FIRST, so it must run FIRST, whatever
// parameter it binds to. With the binding honoured, `y` receives 1 and `x`
// receives 2, so `Pow(x, y)` is 2^1 = 2.
//
// The row's assertions are deliberately COUNT-ROBUST: the LAST "one" must
// precede the LAST "two" (the arm's own evaluation round ran the first
// written argument first), and the program must END with the bound value.
// They do not pin HOW MANY times each expression ran: on this tree the
// generic named-argument pre-pass in `src/codegen/LlvmBackend_Calls2.cpp`
// lowers every supplied argument expression once BEFORE the arm runs and
// DISCARDS the value, so the arm lowers it a second time — a side-effecting
// argument therefore runs TWICE. That double evaluation is a SEPARATE
// defect of the shared pre-pass (it is the subject of #7181) and is not
// pinned here.
const char* kTextualOrder =
    "module M; "
    "class H "
    "  public method One(): int "
    "    Console.WriteLine(\"one\"); "
    "    return 1; "
    "  end One; "
    "  public method Two(): int "
    "    Console.WriteLine(\"two\"); "
    "    return 2; "
    "  end Two; "
    "end H; "
    "class C "
    "  public static method Main(): int "
    "    var h := new H(); "
    "    Console.WriteLine(Math.Pow(y := h.One(), x := h.Two()).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// A WRONGLY-TYPED NAMED ARGUMENT (stdlib §2.2: `Pow(x: double, y: double)`;
// §5.1: a `string` does not implicitly convert to a `double`). This must
// still be REFUSED — the fix must not admit a bad program.
const char* kWrongTypedNamed =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    Console.WriteLine(Math.Pow(y := \"not a double\", x := 3).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The `string` members of the SAME class of defect: the `string` instance
// arms (#7180) and the static `string.Join` arm (#7165, whose separator gate
// is #7191).
//
// NOTE WHAT IS AND IS NOT PINNED HERE. The REORDERED `string.Join` spelling
// (`items := parts, separator := ","`) is REJECTED on this tree by the
// #7150 separator gate in `src/sema/ExprTypecheck_Calls.cpp`, which reads
// `ce.args[0]` positionally; the SEMANTIC half of that pair (#7191) is not
// on this tree, so the reordered spelling cannot be pinned here without
// pinning a REJECTION that #7191's pick removes. It is MEASURED and
// reported to the coordinator instead. What IS pinned here is the #7180
// `string` INSTANCE arms (their reordered spellings ARE admitted on this
// tree) and the `string.Join` spellings that ARE admitted — the named
// spelling already in DECLARATION order and the positional one.
const char* kStringControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.Substring(length := 2, startIndex := 1)); "
    "    Console.WriteLine(s.Replace(newValue := \"X\", oldValue := \"b\")); "
    "    var parts: Array<string> := [\"a\", \"b\", \"c\"]; "
    "    Console.WriteLine(string.Join(separator := \",\", items := parts)); "
    "    Console.WriteLine(string.Join(\",\", parts)); "
    "    return 0; "
    "  end Main; "
    "end C;";

std::string aotStdout(const char* src) {
    bezel::test::JitResult r = runAot(src);
    if (!r.ran) {
        std::fprintf(stderr, "  [did not run] diagnostics:\n");
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "    %s\n", d.message.c_str());
        return std::string();
    }
    return r.stdoutText;
}

}  // namespace

// ===========================================================================
// THE REGRESSION ROWS — the reordered named-argument spelling must produce
// the SAME value as the declaration-order spelling.
// ===========================================================================

// stdlib §2.2: `Pow(x: double, y: double): double`. Before the fix the
// reordered call computed 2^3 = 8 (it bound `y := 2` to `x` and `x := 3` to
// `y`); now it must compute 3^2 = 9.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           pow_reordered_named_args_bind_by_parameter_name_22_5) {
    std::string got = aotStdout(kPowReordered);
    if (got != "9\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("9\n"));
}

// Control: the POSITIONAL spelling and the named spelling already in
// DECLARATION order keep their value, byte for byte.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           pow_positional_and_in_order_named_unchanged_2_2) {
    std::string got = aotStdout(kPowControl);
    if (got != "9\n9\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("9\n9\n"));
}

// stdlib §2.2: `Fmod(x: double, y: double): double` — fmod(10, 4) = 2.
// Before the fix the reordered call computed fmod(4, 10) = 4.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           fmod_reordered_named_args_bind_by_parameter_name_22_5) {
    std::string got = aotStdout(kFmodReordered);
    if (got != "2\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("2\n"));
}

// Control: the POSITIONAL spelling and the named spelling already in
// DECLARATION order keep their value, byte for byte.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           fmod_positional_and_in_order_named_unchanged_2_2) {
    std::string got = aotStdout(kFmodControl);
    if (got != "2\n2\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("2\n2\n"));
}

// stdlib §2.2: `IEEERemainder(x: double, y: double): double` — the IEEE 754
// remainder of 10 by 4 is 2. Before the fix the reordered call computed
// remainder(4, 10) = 4.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           ieee_remainder_reordered_named_args_bind_by_parameter_name_22_5) {
    std::string got = aotStdout(kIeeeReordered);
    if (got != "2\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("2\n"));
}

// Control: the POSITIONAL spelling and the named spelling already in
// DECLARATION order keep their value, byte for byte.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           ieee_remainder_positional_and_in_order_named_unchanged_2_2) {
    std::string got = aotStdout(kIeeeControl);
    if (got != "2\n2\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("2\n2\n"));
}

// stdlib §2.2: `Clamp<T>(value: T, low: T, high: T): T` — Clamp(100, 0, 10)
// is 10. Before the fix the reordered call clamped the HIGH (10) between the
// VALUE (100) and the LOW (0) and returned 0.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           clamp_reordered_named_args_bind_by_parameter_name_22_5) {
    std::string got = aotStdout(kClampReordered);
    if (got != "10\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("10\n"));
}

// Control: the POSITIONAL spelling and the named spelling already in
// DECLARATION order keep their value, byte for byte.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           clamp_positional_and_in_order_named_unchanged_2_2) {
    std::string got = aotStdout(kClampControl);
    if (got != "10\n10\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("10\n10\n"));
}

// stdlib §2.3: `Convert.ToString(value: int, toBase: int): string` —
// ToString(255, 16) is "ff". Before the fix the reordered call took the
// RADIX as the VALUE and the VALUE as the RADIX: it printed "16".
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           convert_to_string_reordered_named_args_bind_by_parameter_name_22_5) {
    std::string got = aotStdout(kToStringReordered);
    if (got != "ff\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("ff\n"));
}

// Control: the POSITIONAL spelling and the named spelling already in
// DECLARATION order keep their value, byte for byte.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           convert_to_string_positional_and_in_order_named_unchanged_2_3) {
    std::string got = aotStdout(kToStringControl);
    if (got != "ff\nff\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("ff\nff\n"));
}

// stdlib §2.2: `Round(x: double, digits: int := 0): double` — Round(2.5, 3)
// is 2.5. Before the fix the reordered call scaled by 10^2.5 (the `digits`
// parameter received the `x` argument) and printed 3.001001499499792.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           round_reordered_named_args_bind_by_parameter_name_22_5) {
    std::string got = aotStdout(kRoundReordered);
    if (got != "2.5\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("2.5\n"));
}

// Control: the POSITIONAL spelling and the named spelling already in
// DECLARATION order keep their value, byte for byte.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           round_positional_and_in_order_named_unchanged_2_2) {
    std::string got = aotStdout(kRoundControl);
    if (got != "2.5\n2.5\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("2.5\n2.5\n"));
}

// ===========================================================================
// THE CONTROLS THAT ARE NOT VALUE-DISCRIMINATING, AND THE NEIGHBOURING
// MEMBERS OF THE SAME CLASS.
// ===========================================================================

// `Min`/`Max` are commutative in value, so no number distinguishes the
// reordered spelling from the declaration-order one. What the row DOES pin
// is that the reordered spelling stays ADMITTED and agrees with the other
// three spellings.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           min_max_reordered_named_call_agrees_with_the_others_2_2) {
    std::string got = aotStdout(kMinMaxReordered);
    if (got != "1\n2\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("1\n2\n"));
}

// Control: the POSITIONAL spellings and the named spellings already in
// DECLARATION order keep their values, byte for byte.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           min_max_positional_and_in_order_named_unchanged_2_2) {
    std::string got = aotStdout(kMinMaxControl);
    if (got != "1\n1\n2\n2\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("1\n1\n2\n2\n"));
}

// THE CALL-SITE TEXTUAL ORDER HALF (§22.5: "A call's supplied argument
// expressions are evaluated in call-site textual order — left to right as
// written, for positional and named arguments alike; naming an argument
// changes which parameter it binds to, never when it runs").
//
// The `y :=` argument is written FIRST, so it must run FIRST. The row
// asserts (a) the first printed tag is the first written argument's, (b) the
// LAST "one" precedes the LAST "two" — the arm's own evaluation round ran
// the first written argument first — and (c) the program ends with the
// BOUND value (2^1 = 2), which is the binding, not the order.
//
// The number of evaluations is deliberately NOT pinned (see the note at the
// head of this file).
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           arguments_evaluate_in_call_site_textual_order_22_5) {
    std::string got = aotStdout(kTextualOrder);
    std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_TRUE(got.rfind("one\n") != std::string::npos);
    BEZEL_EXPECT_TRUE(got.rfind("two\n") != std::string::npos);
    // The last occurrence of each tag: the arm's evaluation round must have
    // run the first written argument ("one") before the second ("two").
    BEZEL_EXPECT_TRUE(got.rfind("one\n") < got.rfind("two\n"));
    // The bound value: y received 1, x received 2, so Pow(x, y) is 2^1 = 2.
    BEZEL_EXPECT_TRUE(got.size() >= 2 &&
                      got.compare(got.size() - 2, 2, "2\n") == 0);
}

// A WRONGLY-TYPED NAMED ARGUMENT must still be REFUSED. stdlib §2.2
// declares `Pow(x: double, y: double)` and §5.1 does not convert a `string`
// to a `double`, so the call is not admissible — the fix must not turn the
// reordered spelling into an admitted one.
//
// MEASURED on the base arm the compiler reports NO §5.1 diagnostic for a
// `Math` built-in argument: the operand reaches codegen and the compiler
// aborts with "IR verification failed" — the POSITIONAL spelling does
// exactly the same, so the two spellings agree. The missing per-argument
// type check is a SEPARATE defect (reported to the coordinator, not fixed
// here) and the row asserts the REFUSAL, not the crash's text.
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           wrongly_typed_named_argument_is_still_refused_5_1) {
    bezel::test::JitResult r = runAot(kWrongTypedNamed);
    // The call must be REFUSED: the program must not run.
    BEZEL_EXPECT_FALSE(r.ran);
    // `r.ran == false` alone cannot tell a COMPILE-TIME REJECTION from a
    // compiler crash, a runtime signal, a timeout or an ASan report (issue
    // #3892), so the row excludes each of those explicitly: what is left is
    // the driver's own compile-time rejection.
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "did not finish within"));
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "AddressSanitizer fired"));
}

// The `string` members of the SAME class of defect — the `string` instance
// arms (#7180) and the static `string.Join` arm (#7165/#7191) — must keep
// working after this change: the reordered `string` instance spellings still
// bind by name ("bc", "aXcdef"), and the admitted `string.Join` spellings
// still print "a,b,c".
BEZEL_TEST(codegen_math_convert_named_arg_binding_7199,
           string_named_args_keep_working_7180_7191) {
    std::string got = aotStdout(kStringControl);
    if (got != "bc\naXcdef\na,b,c\na,b,c\n")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("bc\naXcdef\na,b,c\na,b,c\n"));
}
