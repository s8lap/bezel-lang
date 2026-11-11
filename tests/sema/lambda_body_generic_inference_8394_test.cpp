// narrow #8394 (§14.5 *Inputs and phases*, v0.357). Phase 1: "Each argument
// whose type is self-determined — every non-function-typed argument, a
// function-typed *value*, and a lambda's **annotated** parameter types — is
// unified structurally against its parameter type". Phase 2: "For each
// lambda-literal argument in argument order: the bindings established so far
// are substituted into its parameter's function type; if every parameter
// position of the lambda is then closed, the lambda's parameters take those
// types ... and the body is typed".
//
// A generic callee whose function-typed formal mentions its own type parameter
// (`f: (T) -> R`, `pred: (T) -> bool`, `key: (T) -> K`) gave an untyped lambda
// parameter NO type even after another argument (`xs: Array<T>` <-
// `Array<Box>`) had bound `T`. The body was typed against an untyped `v`:
// `v.Name + "!"` was rejected ("`+` requires both operands to be `string`"),
// and `R` / `K` could not be inferred from the body ("cannot infer the type
// argument"). The sema fix (ExprTypecheck_Exprs.cpp, `inferredFnParamType2`
// and `argOrder2`) types the call's other arguments first (phase 1, wherever
// they sit) and substitutes their bindings into the lambda's formal.
//
// fix #8617 (§14.5, v0.357; owner ruling on #8617, reading A): a callee
// type parameter that occurs only in the lambda's RETURN slot (`key: (T) ->
// K`, the issue's own MaxBy) is bound in phase 2 — "the lambda's inferred
// return type then unifies against the function type's return position,
// binding type parameters still free" — and codegen's monomorphization binds
// it the same way (the #8527 function-typed arm of `applyMethodGenericSubst`,
// LlvmBackend_Calls2.cpp). A later argument at a `K`-bearing position that
// binds nothing — a bare `null`, an empty `[]`, an omitted optional — is then
// checked "at the §22.6 applicability check against the substituted
// signature" (phase 1), so the call is admitted. #8394's narrow guard had
// kept these calls rejected because codegen once had no function-typed arm;
// the `*_admitted_8617` rows pin the ruled answer (`dan` / `cho`), and the
// int / record rows pin that the specialization really binds `K` (before the
// fix they compiled and printed the FIRST element, `ann`).
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
const char kLambdaFirst[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method Sel<T, R>(f: (T) -> R, xs: Array<T>): owned Array<R>
    var r := new Array<R>();
    for var x in xs do r.Add(f(x)); end;
    return r;
  end Sel;
  public static method Keep<T>(pred: (T) -> bool, xs: Array<T>): owned Array<T>
    var r := new Array<T>();
    for var x in xs do if pred(x) then r.Add(x); end; end;
    return r;
  end Keep;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    var s := Seq.Sel(lambda(v) => v.Name + "!", xs);
    for var e in s do Console.Write(e); Console.Write(" "); end; Console.WriteLine("");
    var w := Seq.Keep(lambda(v) => do var t := v.Age > 28; return t; end, xs);
    for var e in w do Console.Write(e.Name); Console.Write(" "); end; Console.WriteLine("");
    var ys := new Array<int>();
    ys.Add(3); ys.Add(41); ys.Add(8); ys.Add(13);
    var d := Seq.Sel(lambda(v) => v * 2, ys);
    for var e in d do Console.Write(e); Console.Write(" "); end; Console.WriteLine("");
  end Main;
end Program;
)";
const char kLaterArgBindsKey[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method MaxByS<T, K>(xs: Array<T>, key: (T) -> K, seed: K): T where K: IComparable<K>
    var best := xs[0];
    var bk := seed;
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end MaxByS;
  public static method Fold<T, A>(f: (A, T) -> A, seed: A, xs: Array<T>): A
    var acc := seed;
    for var x in xs do acc := f(acc, x); end;
    return acc;
  end Fold;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.MaxByS(xs, lambda(v) => v.Age, 0).Name);
    Console.WriteLine(Seq.Fold(lambda(a, v) => a + v.Name.Length, 100, xs));
  end Main;
end Program;
)";
const char kKeyOnlyInFnSlot[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method MaxBy<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end MaxBy;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.MaxBy(xs, lambda(v) => v.Name + "!").Name);
  end Main;
end Program;
)";
const char kKeyFormalGetsNull[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method MaxByNull<T, K>(xs: Array<T>, key: (T) -> K, seed: K?): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end MaxByNull;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.MaxByNull(xs, lambda(v) => v.Name + "!", null).Name);
  end Main;
end Program;
)";
const char kKeyFormalGetsEmptyLiteral[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method MaxByEmpty<T, K>(xs: Array<T>, key: (T) -> K, extra: Array<K>): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end MaxByEmpty;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.MaxByEmpty(xs, lambda(v) => v.Name + "!", []).Name);
  end Main;
end Program;
)";
const char kKeyFormalOmitted[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method MaxByD<T, K>(xs: Array<T>, key: (T) -> K, seed: K? := null): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end MaxByD;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.MaxByD(xs, lambda(v) => v.Name + "!").Name);
  end Main;
end Program;
)";

const char kIntKeyOnlyInFnSlot[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method MaxBy<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end MaxBy;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.MaxBy(xs, lambda(p) => p.Age).Name);
    Console.WriteLine(Seq.MaxBy<Box, int>(xs, lambda(p) => p.Age).Name);
  end Main;
end Program;
)";
const char kIntKeyFormalGetsEmptyLiteral[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K, extra: Array<K>): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, lambda(v) => v.Age, []).Name);
  end Main;
end Program;
)";
const char kIntKeyFormalOmitted[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K, seed: K? := null): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, lambda(v) => v.Age).Name);
  end Main;
end Program;
)";
const char kRecordKeyFormalGetsNull[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public record RK: IComparable<RK>
  public v: int;
  public method CompareTo(other: RK): int
    return this.v - other.v;
  end CompareTo;
end RK;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K, seed: K?): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, lambda(v) => new RK(v.Age), null).Name);
  end Main;
end Program;
)";
const char kInstanceKeyFormalGetsEmptyLiteral[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public class H
  public constructor() end constructor;
  public method M<T, K>(xs: Array<T>, key: (T) -> K, extra: Array<K>): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end M;
end H;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    var h := new H();
    Console.WriteLine(h.M(xs, lambda(v) => v.Name + "!", []).Name);
  end Main;
end Program;
)";
const char kUnqualifiedNullBeforeLambda[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public class Program
  public static method M<T, K>(xs: Array<T>, seed: K?, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end M;
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(M(xs, null, lambda(v) => v.Name + "!").Name);
  end Main;
end Program;
)";
const char kNullConditionalIntKeyOmitted[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public class H
  public constructor() end constructor;
  public method M<T, K>(xs: Array<T>, key: (T) -> K, seed: K? := null): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end M;
end H;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    var h: H? := new H();
    Console.WriteLine(h?.M(xs, lambda(v) => v.Age)?.Name ?? "none");
  end Main;
end Program;
)";

// A §14.5-valid call whose one right answer is `dan` (the greatest of
// `ann!` / `cho!` / `bob!` / `dan!`; the same call with a full explicit
// `<Box, string>` list and `lambda(v: Box)` prints `dan`). (A string key
// returned through `K` leaks in the callee — #8469 — which `runAot` does not
// leak-check; the explicit twin leaks the same bytes.)
void expectAdmittedWithAnswer8617(const char* src, const char* answer) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, answer);
}
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

// §14.5 phase 1 runs over every self-determined argument "wherever it sits":
// `xs` FOLLOWS the lambda and still binds the `T` the lambda's parameter
// takes. Rejected before the fix (`+` on an untyped `v`; `R` not inferable).
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_phase1_binding_argument_after_the_lambda) {
    auto r = runAot(kLambdaFirst);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "ann! cho! bob! dan! \nann cho dan \n6 82 16 26 \n");
}

// Phase 1 binds `K` / `A` from the `seed` argument, which FOLLOWS the lambda
// in the first call and sits between the lambda and `xs` in the second, so
// each lambda's positions are closed at its turn and codegen binds every
// parameter from an argument. Before the fix both calls compiled against an
// untyped `v` and printed `ann` and `0`. (Int keys on purpose: a string key
// returned through `K` leaks in the callee — #8469, unrelated.)
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_phase1_later_argument_binds_the_key_parameter) {
    auto r = runAot(kLaterArgBindsKey);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "cho\n112\n");
}

// fix #8617: `K` occurs only in the lambda's return slot and is bound by
// phase 2 alone. Rejected before the fix (the #8394 guard declined to type
// the lambda: "`+` requires both operands to be `string`").
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_phase2_key_bound_only_by_the_lambda_return_admitted_8617) {
    expectAdmittedWithAnswer8617(kKeyOnlyInFnSlot, "dan\n");
}

// `seed: K?` receives a bare `null`: "a bare `null` literal argument is not
// self-determined and contributes no binding" (§14.5 phase 1) — it binds
// nothing, and converts to the substituted `string?` at the §22.6 check.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_key_formal_given_null_admitted_8617) {
    expectAdmittedWithAnswer8617(kKeyFormalGetsNull, "dan\n");
}

// `extra: Array<K>` receives `[]`, which is typed against the substituted
// `Array<string>` (§15.4 *Empty literals*).
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_key_formal_given_empty_literal_admitted_8617) {
    expectAdmittedWithAnswer8617(kKeyFormalGetsEmptyLiteral, "dan\n");
}

// `seed: K? := null` is omitted: "omitted optional parameters contribute
// nothing" (§14.5 *Inputs and phases*).
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_key_formal_omitted_admitted_8617) {
    expectAdmittedWithAnswer8617(kKeyFormalOmitted, "dan\n");
}

// The int / record key rows: before the fix each compiled against an untyped
// lambda, left `K` at the placeholder in the specialization and printed the
// FIRST element (`ann`); the greatest `Age` is `cho` (40).

// The issue's own #8394 repro: `MaxBy(people, lambda(p) => p.Age)`.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_phase2_int_key_bound_only_by_the_lambda_return_8617) {
    expectAdmittedWithAnswer8617(kIntKeyOnlyInFnSlot, "cho\ncho\n");
}

BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_int_key_formal_given_empty_literal_admitted_8617) {
    expectAdmittedWithAnswer8617(kIntKeyFormalGetsEmptyLiteral, "cho\n");
}

BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_int_key_formal_omitted_admitted_8617) {
    expectAdmittedWithAnswer8617(kIntKeyFormalOmitted, "cho\n");
}

// A record key (`RK: IComparable<RK>`) with `seed: K?` given `null`.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_record_key_formal_given_null_admitted_8617) {
    expectAdmittedWithAnswer8617(kRecordKeyFormalGetsNull, "cho\n");
}

// The instance-method spelling (`h.M(xs, lambda(v) => …, [])`). Rejected
// before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_instance_method_key_formal_given_empty_literal_admitted_8617) {
    expectAdmittedWithAnswer8617(kInstanceKeyFormalGetsEmptyLiteral, "dan\n");
}

// The unqualified spelling with the `null` BEFORE the lambda (`M(xs, null,
// lambda(v) => …)`): phase 1 runs over every argument wherever it sits, and
// the `null` still binds nothing. Rejected before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_unqualified_null_before_the_lambda_admitted_8617) {
    expectAdmittedWithAnswer8617(kUnqualifiedNullBeforeLambda, "dan\n");
}

// The null-conditional spelling (`h?.M(xs, lambda(v) => v.Age)?.Name`) with
// the optional `seed` omitted. Printed `ann` before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_null_conditional_int_key_formal_omitted_admitted_8617) {
    expectAdmittedWithAnswer8617(kNullConditionalIntKeyOmitted, "cho\n");
}
