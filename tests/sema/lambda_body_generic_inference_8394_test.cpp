// narrow #8394 (§14.5 phase 2, v0.357): "For each lambda-literal argument in
// argument order: the bindings established so far are substituted into its
// parameter's function type; if every parameter position of the lambda is
// then closed, the lambda's parameters take those types ... and the body is
// typed." A generic callee whose function-typed formal mentions its own type
// parameter (`f: (T) -> R`, `pred: (T) -> bool`, `key: (T) -> K`) gave an
// untyped lambda parameter NO type even after an earlier argument
// (`xs: Array<T>` <- `Array<Box>`) had bound `T`. The body was typed against an
// untyped `v`: `v.Name + "!"` was rejected ("`+` requires both operands to be
// `string`"), and `R` / `K` could not be inferred from the body ("cannot infer
// the type argument"). The sema fix substitutes the earlier arguments'
// bindings into the formal (ExprTypecheck_Exprs.cpp, inferredFnParamType2).
//
// NARROW: the issue's own MaxBy / MinBy / OrderBy rows are NOT fixed here. Their
// `K` occurs only in the lambda's return slot, and codegen's
// monomorphization (`applyMethodGenericSubst` / `unifyMethodReturnTypeArg`,
// LlvmBackend_Calls2.cpp) has no function-typed arm, so `K` stays unbound in the
// specialization. The sema half deliberately declines such a callee so it does
// not turn a rejected program into a silently wrong one.
//
// The int rows build `xs` with `Add` rather than a literal: a literal-built
// local `Array<int>` passed to a generic static method leaks (#7701), which
// would make these rows flaky under the ASan oracle for a reason unrelated to
// this fix.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
const char kClassExpr[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method Select<T, R>(xs: Array<T>, f: (T) -> R): owned Array<R>
    var r := new Array<R>();
    for var x in xs do r.Add(f(x)); end;
    return r;
  end Select;
  public static method Where<T>(xs: Array<T>, pred: (T) -> bool): owned Array<T>
    var r := new Array<T>();
    for var x in xs do if pred(x) then r.Add(x); end; end;
    return r;
  end Where;
  public static method GroupBy<T, K>(xs: Array<T>, key: (T) -> K): owned Dictionary<K, int> where K: hashable
    var d := new Dictionary<K, int>();
    for var x in xs do
      var k := key(x);
      d[k] := (d.GetOrNull(k) ?? 0) + 1;
    end;
    return d;
  end GroupBy;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    var s := Seq.Select(xs, lambda(v) => v.Name + "!");
    for var e in s do Console.Write(e); Console.Write(" "); end; Console.WriteLine("");
    var w := Seq.Where(xs, lambda(v) => v.Age > 28);
    for var e in w do Console.Write(e.Name); Console.Write(" "); end; Console.WriteLine("");
    var g := Seq.GroupBy(xs, lambda(v) => v.Age / 10);
    Console.WriteLine($"{g.Count} {g[3]}");
  end Main;
end Program;
)";
const char kClassBlock[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method Select<T, R>(xs: Array<T>, f: (T) -> R): owned Array<R>
    var r := new Array<R>();
    for var x in xs do r.Add(f(x)); end;
    return r;
  end Select;
  public static method Where<T>(xs: Array<T>, pred: (T) -> bool): owned Array<T>
    var r := new Array<T>();
    for var x in xs do if pred(x) then r.Add(x); end; end;
    return r;
  end Where;
  public static method GroupBy<T, K>(xs: Array<T>, key: (T) -> K): owned Dictionary<K, int> where K: hashable
    var d := new Dictionary<K, int>();
    for var x in xs do
      var k := key(x);
      d[k] := (d.GetOrNull(k) ?? 0) + 1;
    end;
    return d;
  end GroupBy;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    var s := Seq.Select(xs, lambda(v) => do var t := v.Name + "!"; return t; end);
    for var e in s do Console.Write(e); Console.Write(" "); end; Console.WriteLine("");
    var w := Seq.Where(xs, lambda(v) => do var t := v.Age > 28; return t; end);
    for var e in w do Console.Write(e.Name); Console.Write(" "); end; Console.WriteLine("");
    var g := Seq.GroupBy(xs, lambda(v) => do var t := v.Age / 10; return t; end);
    Console.WriteLine($"{g.Count} {g[3]}");
  end Main;
end Program;
)";
const char kIntExpr[] = R"(
module P;
public static class Seq
  public static method Select<T, R>(xs: Array<T>, f: (T) -> R): owned Array<R>
    var r := new Array<R>();
    for var x in xs do r.Add(f(x)); end;
    return r;
  end Select;
  public static method Where<T>(xs: Array<T>, pred: (T) -> bool): owned Array<T>
    var r := new Array<T>();
    for var x in xs do if pred(x) then r.Add(x); end; end;
    return r;
  end Where;
  public static method GroupBy<T, K>(xs: Array<T>, key: (T) -> K): owned Dictionary<K, int> where K: hashable
    var d := new Dictionary<K, int>();
    for var x in xs do
      var k := key(x);
      d[k] := (d.GetOrNull(k) ?? 0) + 1;
    end;
    return d;
  end GroupBy;
end Seq;
public class Program
  public static method Main()
    var xs := new Array<int>();
    xs.Add(3); xs.Add(41); xs.Add(8); xs.Add(13);
    var s := Seq.Select(xs, lambda(v) => v * 2);
    for var e in s do Console.Write(e); Console.Write(" "); end; Console.WriteLine("");
    var w := Seq.Where(xs, lambda(v) => v > 5);
    for var e in w do Console.Write(e); Console.Write(" "); end; Console.WriteLine("");
    var g := Seq.GroupBy(xs, lambda(v) => v mod 2);
    Console.WriteLine($"{g.Count} {g[1]}");
  end Main;
end Program;
)";
const char kIntBlock[] = R"(
module P;
public static class Seq
  public static method Select<T, R>(xs: Array<T>, f: (T) -> R): owned Array<R>
    var r := new Array<R>();
    for var x in xs do r.Add(f(x)); end;
    return r;
  end Select;
  public static method Where<T>(xs: Array<T>, pred: (T) -> bool): owned Array<T>
    var r := new Array<T>();
    for var x in xs do if pred(x) then r.Add(x); end; end;
    return r;
  end Where;
  public static method GroupBy<T, K>(xs: Array<T>, key: (T) -> K): owned Dictionary<K, int> where K: hashable
    var d := new Dictionary<K, int>();
    for var x in xs do
      var k := key(x);
      d[k] := (d.GetOrNull(k) ?? 0) + 1;
    end;
    return d;
  end GroupBy;
end Seq;
public class Program
  public static method Main()
    var xs := new Array<int>();
    xs.Add(3); xs.Add(41); xs.Add(8); xs.Add(13);
    var s := Seq.Select(xs, lambda(v) => do var t := v * 2; return t; end);
    for var e in s do Console.Write(e); Console.Write(" "); end; Console.WriteLine("");
    var w := Seq.Where(xs, lambda(v) => do var t := v > 5; return t; end);
    for var e in w do Console.Write(e); Console.Write(" "); end; Console.WriteLine("");
    var g := Seq.GroupBy(xs, lambda(v) => do var t := v mod 2; return t; end);
    Console.WriteLine($"{g.Count} {g[1]}");
  end Main;
end Program;
)";
}  // namespace

// Class element, expression lambdas. Rejected before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_phase2_class_element_expression_lambdas) {
    auto r = runAot(kClassExpr);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ann! cho! bob! dan! \nann cho dan \n3 2\n");
}

// Class element, block lambdas. Rejected before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_phase2_class_element_block_lambdas) {
    auto r = runAot(kClassBlock);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ann! cho! bob! dan! \nann cho dan \n3 2\n");
}

// int element, expression lambdas. Rejected before the fix (`R` / `K` could
// not be inferred).
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_phase2_int_element_expression_lambdas) {
    auto r = runAot(kIntExpr);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6 82 16 26 \n41 8 13 \n2 3\n");
}

// int element, block lambdas: a control that already passed before the fix
// and must stay green.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_phase2_int_element_block_lambdas_control) {
    auto r = runAot(kIntBlock);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6 82 16 26 \n41 8 13 \n2 3\n");
}
