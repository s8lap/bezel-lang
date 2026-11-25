// Regression tests for issue #9185 — the `.Length` member read of a captured
// `FrozenArray<T>` had NO TYPE in sema, so a lambda body that read it had no
// inferred return type and the consuming statement was silently dropped.
//
// THE DEFECT. stdlib §4.19 declares `public readonly property Length: int;`
// on `FrozenArray<T>`. A builtin reaches codegen in TWO TypeRef shapes: the
// DECLARATION-LESS one (an annotation — `var f: FrozenArray<int> := ...`) and
// the BUILTIN-CLASS-STUB one (a factory result — `var f := FrozenArray<int>.
// From(s);`, whose `declaration` is the synthetic `builtinClassSym`). The
// declaration-less shape's int-property arm lives INSIDE the `!targetType`
// block of `ExprTC::resolveMemberAccess` — the block that serves a receiver
// carrying NO TypeSymbol — so the STUB shape never reached it, and
// `f.Length` was typed for the annotation spelling and NOT for the factory
// spelling: the same expression with a type in one spelling and none in the
// other.
//
// WHY THAT IS A SILENT MISCOMPILE AND NOT A REJECTION. Language spec §27.3
// (v0.359): "Expression-bodied lambdas need no inference rule: the body
// expression's type is the return type". With the body's type missing,
// `var fn := lambda() => f.Length;` got a function type with NO
// `funcReturn` — and codegen's closure call (`emitClosureCall`) builds the
// call's return type from `rt->funcReturn`, so the call was typed VOID, it
// produced no value, and the enclosing statement was DROPPED: the #3079
// internal warning ("the argument of this `Console.WriteLine` failed to lower
// to a value, so the whole statement emits nothing — not even the line
// terminator"), the WRONG output, and exit 0.
//
// The fix types the stub-shaped receiver's int property (stdlib §4.19's
// `Length: int`), so the lambda's return type is inferred and the call is
// typed. The rows below are RED on the base for exactly that reason: the
// captured read's line is missing from the output.
//
// Spec: §27.3 (lambda bodies are expressions evaluated with their captures;
// the expression body's type is the return type), stdlib §4.19 (the
// `FrozenArray<T>` declaration block: `public readonly property Length: int;`
// and `public property [i: int]: T`), language spec §8.1 (the universal
// `Object` members). Spec revision v0.359.
//
// Model: none — this is a typing gap in the compiler's own member resolution,
// not a stdlib surface question. The `FrozenArray<string>` and the nested
// lambda rows are the issue's own siblings.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The issue's own repro, verbatim: a captured `FrozenArray<int>` local read
// through a lambda. The base prints `2` (the direct read only) and drops the
// lambda call's line entirely; the fixed compiler prints `2` and `2`.
const char* kCapturedLength =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var s := new Array<int>(); s.Add(7); s.Add(2);\n"
    "    using var f := FrozenArray<int>.From(s);\n"
    "    var fn := lambda() => f.Length;\n"
    "    Console.WriteLine(f.Length);\n"
    "    Console.WriteLine(fn());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The issue's second shape: the same captured read inside a SUM, which on the
// base drew the #2961 "store dropped" warning and left the total at 0.
const char* kCapturedLengthInSum =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var s := new Array<int>(); s.Add(7); s.Add(2);\n"
    "    using var f := FrozenArray<int>.From(s);\n"
    "    var fn := lambda() => f.Length;\n"
    "    var tot := 0;\n"
    "    tot := tot + fn();\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The `FrozenArray<string>` sibling: stdlib §4.19 admits `string` elements,
// and the element type does not change the property's declared type.
const char* kCapturedStringLength =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var s := new Array<string>(); s.Add(\"aa\"); s.Add(\"bb\");\n"
    "    using var f := FrozenArray<string>.From(s);\n"
    "    var fn := lambda() => f.Length;\n"
    "    Console.WriteLine(fn());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The NESTED-lambda sibling: the inner lambda's body reads the captured
// `FrozenArray`, so the inner closure's return type is the one inferred.
const char* kNestedLambda =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var s := new Array<int>(); s.Add(7); s.Add(2);\n"
    "    using var f := FrozenArray<int>.From(s);\n"
    "    var outer := lambda() => lambda() => f.Length;\n"
    "    Console.WriteLine(outer()());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL (green on the base): the issue's own working control — the
// INDEXER read of the same captured `FrozenArray` (stdlib §4.19's
// `public property [i: int]: T`) was already typed, so it is unaffected.
const char* kCapturedIndexer =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var s := new Array<int>(); s.Add(7); s.Add(2);\n"
    "    using var f := FrozenArray<int>.From(s);\n"
    "    var fn := lambda() => f[0];\n"
    "    Console.WriteLine(fn());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL (green on the base): the `Count` read of a captured `Array<int>`
// (stdlib §4.4's `Count`) is a different property on a different head and is
// unaffected.
const char* kCapturedCount =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var s := new Array<int>(); s.Add(7); s.Add(2);\n"
    "    var fn := lambda() => s.Count;\n"
    "    Console.WriteLine(fn());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

void expectOutput(const char* src, const char* out) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

// The issue's own repro: the captured `FrozenArray`'s `Length` read has the
// stdlib §4.19 declared type, so the lambda that reads it returns that
// length and BOTH lines are printed.
BEZEL_TEST(frozen_array_length_captured_lambda_9185,
           captured_length_read_prints_the_declared_length_spec_4_19) {
    expectOutput(kCapturedLength, "2\n2\n");
}

// The issue's second shape: the captured read inside a sum keeps the total.
BEZEL_TEST(frozen_array_length_captured_lambda_9185,
           captured_length_in_a_sum_keeps_the_total_spec_27_3) {
    expectOutput(kCapturedLengthInSum, "2\n");
}

// The `FrozenArray<string>` sibling: the element type does not change the
// property's declared type.
BEZEL_TEST(frozen_array_length_captured_lambda_9185,
           captured_string_length_read_prints_the_length_spec_4_19) {
    expectOutput(kCapturedStringLength, "2\n");
}

// The nested-lambda sibling: the INNER closure's inferred return type is the
// captured read's type.
BEZEL_TEST(frozen_array_length_captured_lambda_9185,
           nested_lambda_over_a_captured_frozen_array_spec_27_3) {
    expectOutput(kNestedLambda, "2\n");
}

// CONTROL: the captured INDEXER read of the same `FrozenArray` is unchanged.
BEZEL_TEST(frozen_array_length_captured_lambda_9185,
           control_captured_indexer_read_is_unchanged_spec_4_19) {
    expectOutput(kCapturedIndexer, "7\n");
}

// CONTROL: a captured `Array<int>`'s `Count` read is unchanged.
BEZEL_TEST(frozen_array_length_captured_lambda_9185,
           control_captured_array_count_read_is_unchanged_spec_4_4) {
    expectOutput(kCapturedCount, "2\n");
}
