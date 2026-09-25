// Regression tests for #4896 — an inferred lambda parameter whose type is
// a generic method's type parameter, specialized at `char` or at an enum,
// interpolated as its underlying INTEGER value instead of calling
// `ToString()` (silent wrong value, exit 0, no diagnostic).
//
// ISSUE REPRO (verbatim from #4896, corrected per #4927):
//
//   module Conf;
//   public enum Color Red, Green; end Color;
//   public class Util
//     public static method Map<W>(x: W, f: (W) -> string): string
//       return f(x);
//     end Map;
//     public static method Go<T>(v: T): string
//       return Map(v, lambda(y) => $"<{y}>");
//     end Go;
//   end Util;
//   public class Program
//     public static method Main()
//       Console.WriteLine(Util.Go<char>('x'));
//       Console.WriteLine(Util.Go<Color>(Color.Green));
//     end Main;
//   end Program;
//
// Observed pre-fix: `<120>` then `<1>` (the raw code point / ordinal).
// Expected per stdlib §2.13 + §8.5 (v0.307): `<x>` (char.ToString() is the
// character itself) then `<Conf.Color>` (enum ToString() renders the
// QUALIFIED TYPE NAME, not the constant identifier — #4927's correction;
// the identifier direction is `enum_value.NameOf()`, tracked separately by
// #3862).
//
// ROOT CAUSE. `char` and every enum both lower to LLVM `i32` — the SAME
// LLVM type the generic-substitution machinery uses throughout
// `Emitter::lowerCallDispatch`'s pre-pass (LlvmBackend_Calls2.cpp) and
// `Emitter::lowerLambda`'s hint injection (LlvmBackend_Exprs2.cpp) as the
// sentinel for "still-unresolved type parameter". `lt == i32()` is
// therefore ambiguous: it is true both for a genuinely free type parameter
// AND for one resolved to `char`/an enum, so the pre-pass never recorded a
// substitution for those two cases, `lowerLambda` never got a concrete
// type hint for the inferred parameter `y`, and interpolation's `y.ToString()`
// dispatch (§18.5) fell back to the raw-int path.
//
// THE FIX (LlvmBackend_Calls2.cpp, both pre-pass sites; LlvmBackend_Exprs2.cpp,
// `lowerLambda`'s hint site) breaks the ambiguity by re-substituting the
// argument's sema TypeRef through the ACTIVE `genericSubstSemaTy_` map and
// checking whether the result is still a BARE type-parameter reference
// (still-unresolved) or a concrete type (resolved to char/enum) — per §14.5
// phase 2, which requires the lambda's parameter type to be taken from the
// bindings established so far, whatever LLVM shape they lower to.
//
// SCOPE NOTE — a separate, pre-existing defect this fix's own investigation
// surfaced but does NOT fix: compiling the issue's literal two-call repro
// (`Go<char>` then `Go<Color>` in ONE program) still prints `<x>` then `<>`
// (the second call's inferred-parameter interpolation comes out EMPTY, not
// merely unfixed) — MEASURED, and NOT specific to char/enum: an unrelated
// `Go<int>(5)` followed by `Go<string>("hi")` in one program (neither an
// i32/placeholder collision at all) shows the identical failure shape,
// printing `<5>` then a raw-pointer value reinterpreted as a decimal
// instead of `<hi>`. Both rows are `Go<T>`'s SAME lambda AST (same
// `ast::LambdaExpr`, same `ParameterSymbol* par` for `y`) monomorphized
// twice in one compile — a cross-specialization state leak/staleness
// distinct from the i32-collision this issue is about (confirmed via
// tracing: the type-hint computation itself is independently correct and
// distinct for EACH specialization at the metadata level; the corruption
// surfaces downstream of it). This class of defect — the same lambda AST
// revisited across sibling monomorphizations of its enclosing generic
// method — is DISTINCT from #4813 (a same-NAME type-parameter collision;
// this repro's `Go<T>`/`Map<W>` use different names) and is filed
// separately as issue #5007, which also has a repro unrelated to char/enum
// entirely (`Go<int>` then `Go<string>`), showing this is a pre-existing
// defect in the generic-substitution-across-specializations machinery that
// #4896's own fix merely makes reachable for a NEW pair of inputs, not one
// it introduces. Per CLAUDE.md's "decline-with-reasons" discipline this is
// reported rather than silently pulled into this fix's scope: root-causing
// it needs inspecting the actual emitted LLVM IR across both
// specializations, which is unrelated, open-ended infrastructure work. The
// tests below therefore exercise the SAME underlying bug
// #4896 describes (a generic method's inferred lambda parameter,
// specialized at char/enum, must interpolate via ToString) without relying
// on two sibling monomorphizations of one lambda AST in a single compile —
// each nested (`Go<T>`) row runs alone, and a same-compile two-row control
// uses two syntactically DISTINCT lambda literals (no AST sharing) to
// confirm the fix generalizes within one program too.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

// #4896 / §14.5 phase 2, §18.5, stdlib §8.5 (v0.307): a generic method's
// inferred lambda parameter specialized at `char`, alone in its own
// compile (avoids the separate cross-specialization leak documented
// above). Pre-fix stdout: `<120>\n`. Post-fix: `<x>\n`.
BEZEL_TEST(inferred_lambda_param_char_enum_interpolation_4896,
           nested_generic_char_specialization_prints_char) {
    auto r = runAot(
        "module Conf; "
        "public class Util "
        "  public static method Map<W>(x: W, f: (W) -> string): string "
        "    return f(x); "
        "  end Map; "
        "  public static method Go<T>(v: T): string "
        "    return Map(v, lambda(y) => $\"<{y}>\"); "
        "  end Go; "
        "end Util; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Util.Go<char>('x')); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "<x>\n");
}

// #4896 / §14.5 phase 2, §18.5, stdlib §2.13 + §8.5 (v0.307, corrected per
// #4927): the same shape specialized at an enum, alone in its own compile.
// Enum `ToString()` renders the QUALIFIED TYPE NAME, not the constant
// identifier. Pre-fix stdout: `<1>\n` (the ordinal). Post-fix: `<Conf.Color>\n`.
BEZEL_TEST(inferred_lambda_param_char_enum_interpolation_4896,
           nested_generic_enum_specialization_prints_qualified_name) {
    auto r = runAot(
        "module Conf; "
        "public enum Color Red, Green; end Color; "
        "public class Util "
        "  public static method Map<W>(x: W, f: (W) -> string): string "
        "    return f(x); "
        "  end Map; "
        "  public static method Go<T>(v: T): string "
        "    return Map(v, lambda(y) => $\"<{y}>\"); "
        "  end Go; "
        "end Util; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Util.Go<Color>(Color.Green)); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "<Conf.Color>\n");
}

// #4896 regression control: the SAME nested shape specialized at `int` must
// stay unaffected — `int` also lowers to LLVM i32 (the same ambiguous
// sentinel this fix disambiguates), so this pins that the fix does not
// regress the pre-existing, already-correct int path. Unchanged on both
// sides of the fix: `<5>\n`.
BEZEL_TEST(inferred_lambda_param_char_enum_interpolation_4896,
           nested_generic_int_specialization_unaffected) {
    std::string out = runAotOut(
        "module Conf; "
        "public class Util "
        "  public static method Map<W>(x: W, f: (W) -> string): string "
        "    return f(x); "
        "  end Map; "
        "  public static method Go<T>(v: T): string "
        "    return Map(v, lambda(y) => $\"<{y}>\"); "
        "  end Go; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Go<int>(5)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(out, "<5>");
}

// #4896, same-compile control: two syntactically DISTINCT lambda literals
// (direct, non-nested `Map<W>` calls — no shared `ast::LambdaExpr`, so the
// cross-specialization leak documented in the file header does not apply)
// specialized at char then at an enum in ONE program. Confirms the fix
// generalizes within a single compile when the ambiguity-breaking
// substitution is not asked to serve two sibling monomorphizations of the
// same lambda AST. Pre-fix stdout: `<120>\n<1>\n`. Post-fix:
// `<x>\n<Conf.Color>\n`.
BEZEL_TEST(inferred_lambda_param_char_enum_interpolation_4896,
           direct_call_char_then_enum_same_compile) {
    auto r = runAot(
        "module Conf; "
        "public enum Color Red, Green; end Color; "
        "public class Util "
        "  public static method Map<W>(x: W, f: (W) -> string): string "
        "    return f(x); "
        "  end Map; "
        "end Util; "
        "public class Program "
        "  public static method Main() "
        "    var c: char := 'x'; "
        "    var col: Color := Color.Green; "
        "    Console.WriteLine(Util.Map(c, lambda(y) => $\"<{y}>\")); "
        "    Console.WriteLine(Util.Map(col, lambda(y) => $\"<{y}>\")); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "<x>\n<Conf.Color>\n");
}
