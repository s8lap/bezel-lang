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

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
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

// ---------------------------------------------------------------------------
// narrow #8394, second residual pass (§14.5 / §22.6, v0.357).
//
// (a) An OVERLOADED name. §14.5: "Inference runs after arity filtering and
// named-argument binding (§22.5) fix the argument-to-parameter
// correspondence". When the §22.5 binding leaves exactly ONE candidate, its
// formal is the lambda's formal, and phase 2 types the lambda against it.
// The guard required a sole same-name method, so a sibling of another arity
// (or another generic arity, or another receiver spelling) left `v` untyped:
// an int key compiled with `K` at the placeholder and printed the FIRST
// element (`ann`), a string body was rejected ("`+` requires both operands to
// be `string`"). Same-ARITY overloads are not taken (each candidate's lambda
// typing is §22.6's per-candidate step) and stay a residual.
//
// (b) A full EXPLICIT list. §14.5 *Explicit lists are all-or-nothing*: "When
// a full explicit list is supplied, no inference runs" — the listed types ARE
// the bindings, so `Seq.Select<Box, string>(xs, lambda(v) => v.Name + "?")`
// types `v` as `Box`. It was left untyped: the call was rejected, a body
// naming a member `Box` lacks compiled silently, and a body returning `int`
// at a `string` key reached the IR verifier.
//
// (c) A NULLABLE binding. Phase 1 binds a bare `T` from `Array<int?>` to
// `int?`, and the lambda's parameter takes it. The guard refused a nullable
// bare-`T` binding (the #5117 fix-forward's shape, which it measured on a
// §7.4-NARROWED argument, still refused here), so `v` was lowered at the
// placeholder: three garbage ints instead of `3 -1 8`, an empty line for a
// `Box?` element, an IR verification failure for a `string?` element.
// ---------------------------------------------------------------------------

namespace {
const char kOvlArityIntKey[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;
  public static method M(a: int, b: int, c: int, d: int): int
    return a + b + c + d;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, lambda(v) => v.Age).Name);
  end Main;
end Program;
)";
const char kOvlArityStringBody[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;
  public static method M(a: int, b: int, c: int, d: int): int
    return a + b + c + d;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, lambda(v) => (v.Name + "!!!!!!!").Length - v.Age).Name);
  end Main;
end Program;
)";
const char kOvlNamedKey[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;
  public static method M(a: int, b: int, c: int, d: int): int
    return a + b + c + d;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, key := lambda(v) => v.Age).Name);
  end Main;
end Program;
)";
const char kOvlInstance[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public class Picker
  public method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;
  public method M(a: string): int
    return a.Length;
  end M;
end Picker;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    var p := new Picker();
    Console.WriteLine(p.M(xs, lambda(v) => v.Age).Name);
  end Main;
end Program;
)";
const char kOvlTwoGenerics[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;
  public static method M<T>(xs: Array<T>, a: int, b: int): T
    return xs[a + b];
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, lambda(v) => v.Age).Name);
  end Main;
end Program;
)";
const char kOvlExplicitListStringBody[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;
  public static method M(a: int, b: int): int
    return a + b;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M<Box, int>(xs, lambda(v) => (v.Name + "!!!!!!!").Length - v.Age).Name);
  end Main;
end Program;
)";
const char kExplicitSelectClass[] = R"(
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
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    var s := Seq.Select<Box, string>(xs, lambda(v) => v.Name + "?");
    for var e in s do Console.Write(e); end; Console.WriteLine("");
  end Main;
end Program;
)";
const char kExplicitSelectIntElem[] = R"(
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
end Seq;
public class Program
  public static method Main()
    var ys := new Array<int>(); ys.Add(4); ys.Add(9);
    var s := Seq.Select<int, string>(ys, lambda(v) => v.ToString() + "!");
    for var e in s do Console.Write(e); end; Console.WriteLine("");
  end Main;
end Program;
)";
const char kExplicitNoSuchMember[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M<Box, string>(xs, lambda(v) => v.Nope).Name);
  end Main;
end Program;
)";
const char kExplicitWrongReturn[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M<Box, string>(xs, lambda(v) => v.Age).Name);
  end Main;
end Program;
)";
const char kNullableIntElem[] = R"(
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
end Seq;
public class Program
  public static method Main()
    var ns := new Array<int?>();
    ns.Add(3); ns.Add(null); ns.Add(8);
    var s := Seq.Select(ns, lambda(v) => v ?? -1);
    for var e in s do Console.Write(e); Console.Write(" "); end; Console.WriteLine("");
  end Main;
end Program;
)";
const char kNullableIntBlock[] = R"(
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
end Seq;
public class Program
  public static method Main()
    var ns := new Array<int?>();
    ns.Add(3); ns.Add(null); ns.Add(8);
    var s := Seq.Select(ns, lambda(v) => do if v <> null then return v * 2; end; return 0; end);
    for var e in s do Console.Write(e); Console.Write(" "); end; Console.WriteLine("");
  end Main;
end Program;
)";
const char kNullableStringElem[] = R"(
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
end Seq;
public class Program
  public static method Main()
    var ss := new Array<string?>(); ss.Add("a"); ss.Add(null);
    var s := Seq.Select(ss, lambda(v) => v ?? "-");
    for var e in s do Console.Write(e); end; Console.WriteLine("");
  end Main;
end Program;
)";
const char kNullableClassElem[] = R"(
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
end Seq;
public class Program
  public static method Main()
    var bs := new Array<Box?>(); var q := new Box("q", 1); bs.Add(q); bs.Add(null);
    var s := Seq.Select(bs, lambda(v) => v?.Name ?? "-");
    for var e in s do Console.Write(e); end; Console.WriteLine("");
  end Main;
end Program;
)";

void expectAnswer8394b(const char* src, const char* answer) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, answer);
}
}  // namespace

// (a) `Seq.M(xs, lambda(v) => v.Age)` beside a four-`int` `M`: the greatest
// `Age` is `cho` (40). Printed `ann` before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_overloaded_arity_unique_int_key) {
    expectAnswer8394b(kOvlArityIntKey, "cho\n");
}

// (a) A string-typed body (`(v.Name + "!!!!!!!").Length - v.Age`: the
// smallest `Age` wins, `bob`). Rejected before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_overloaded_arity_unique_string_body) {
    expectAnswer8394b(kOvlArityStringBody, "bob\n");
}

// (a) The lambda passed by name (`key := lambda(v) => v.Age`): §22.5 binds it
// to `key` before inference. Printed `ann` before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_overloaded_named_lambda_argument) {
    expectAnswer8394b(kOvlNamedKey, "cho\n");
}

// (a) The instance spelling, beside a one-`string` `M`. Printed `ann`.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_overloaded_instance_method) {
    expectAnswer8394b(kOvlInstance, "cho\n");
}

// (a) Beside a second GENERIC `M<T>` of another arity. Printed `ann`.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_overloaded_beside_another_generic) {
    expectAnswer8394b(kOvlTwoGenerics, "cho\n");
}

// (a)+(b) Overloaded AND a full list (`M<Box, int>`): "Where the name is
// overloaded, the list's length selects first". Rejected before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_overloaded_explicit_list_string_body) {
    expectAnswer8394b(kOvlExplicitListStringBody, "bob\n");
}

// (b) `Select<Box, string>`: rejected before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_explicit_list_class_element) {
    expectAnswer8394b(kExplicitSelectClass, "ann?cho?bob?dan?\n");
}

// (b) `Select<int, string>` over an `Add`-built `Array<int>` (#7701 avoids a
// literal). Rejected before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_explicit_list_int_element) {
    expectAnswer8394b(kExplicitSelectIntElem, "4!9!\n");
}

// (b) §11.3: `v.Nope` names no member of `Box`. Compiled and printed `ann`
// before the fix (a silent acceptance).
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_explicit_list_body_member_lookup_rejected) {
    auto r = runAot(kExplicitNoSuchMember);
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "no member `Nope` on type `Box`"));
}

// (b) §5.1: an `int` body at the listed `string` key. Reached the IR verifier
// before the fix ("IR verification failed").
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_explicit_list_wrong_return_rejected) {
    auto r = runAot(kExplicitWrongReturn);
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the lambda body's returned value has a type that is not implicitly "
           "convertible to the target type"));
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(r, "IR verification failed"));
}

// (c) `Select(ns, lambda(v) => v ?? -1)` over `Array<int?>`. Printed three
// garbage ints before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_nullable_int_element_coalesce) {
    expectAnswer8394b(kNullableIntElem, "3 -1 8 \n");
}

// (c) A block body that narrows `v` (§7.4) before `v * 2`. Garbage before.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_nullable_int_element_block_narrowing) {
    expectAnswer8394b(kNullableIntBlock, "6 0 16 \n");
}

// (c) A `string?` element. IR verification failure before the fix.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_nullable_string_element_coalesce) {
    expectAnswer8394b(kNullableStringElem, "a-\n");
}

// (c) A `Box?` element through `v?.Name ?? "-"`. Printed an empty line before.
BEZEL_TEST(sema_lambda_body_generic_inference_8394,
           spec_14_5_nullable_class_element_null_conditional) {
    expectAnswer8394b(kNullableClassElem, "q-\n");
}
