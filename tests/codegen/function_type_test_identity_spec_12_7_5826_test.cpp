// Regression pins for issue #5826 (spec §12.7 "Type tests", *Function-typed
// targets*, v0.338): `is` / `is not` / `as` against a function type.
//
// §12.7: "A test whose target is a function type … is decided by **identity
// of the function type**, not by §27.1.1's assignability: it succeeds exactly
// when the operand holds a function value whose own function type is
// identical to the target's under §27.1's identity (slot types, modifiers and
// return type; parameter names and the function type's own nullability
// disregarded). A function value's own type is the type it was **formed**
// at — a lambda literal's written parameter types and return type, the
// target's where inferred".
//
// Before the fix a function value carried no type of its own, so such a test
// did not lower at all: as an `if` condition it read as false whatever the
// value, and as a value (`Console.WriteLine(o is (int) -> int)`, `var g := o
// as (int) -> int`) it emitted nothing, behind an `internal:` warning. Every
// row prints at least one `True` / a successful `as`, so each fails pre-fix.
//
// ORACLES. Rows that copy a function value out of a function-typed local
// leak that cell with or without this fix (the #5967 family), so they run on
// `runAot`; `as_round_trip_is_sanitizer_clean_12_7` keeps to reclaimed shapes
// and runs looped on `runAotChecked` (generated-code ASan + LSan), where an
// `as` that handed back a wrongly-offset or over-released reference would be
// reported.
//
// NOT pinned: the formed type of a METHOD REFERENCE whose target differs from
// the method's own signature by a §27.1.1 conversion (`var m: (Dog) -> () :=
// Util.Feed;` over `Feed(a: Animal)`). §12.7 points at §27.6 for it and §27.6
// does not say which of the two types it is; rows here use references whose
// target equals the method's signature.

#include <string>

#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kDecls =
    "module M; "
    "public class Animal "
    "  public constructor() end constructor; "
    "end Animal; "
    "public class Dog: Animal "
    "  public constructor() end constructor; "
    "end Dog; "
    "public class Acc "
    "  public _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method Add(x: int): int => x + _n; "
    "end Acc; "
    "public static class Util "
    "  public static method Twice(n: int): int => n * 2; "
    "  public static method Len(s: string): int => s.Length; "
    "  public static method MaybeLen(s: string?): int => 0; "
    "  public static method Bump(ref n: int) n := n + 1; end Bump; "
    "  public static method Take(n: int) end Take; "
    "  public static method Is<T>(o: Object): bool => o is T; "
    "  public static method Casts<T>(o: Object): bool => (o as T) <> null; "
    "  public static method IsIntToInt(o: Object): bool => o is (int) -> int; "
    "end Util; ";

std::string mainWith(const std::string& body) {
    return std::string(kDecls) +
           "public class Program "
           "  public static method Main() " + body +
           "  end Main; "
           "end Program;";
}

}  // namespace

// §12.7's own example, verbatim: "over `f: (Animal) -> ()`, after `var g:
// (Dog) -> () := f; var o: Object := g;`, `o is (Animal) -> ()` is `true` and
// `o is (Dog) -> ()` is `false`." The value was formed at the lambda's
// WRITTEN parameter type; the §27.1.1 retyping to `(Dog) -> ()` is a view.
BEZEL_TEST(function_type_test_identity_spec_12_7_5826,
          spec_example_retyped_value_keeps_its_formed_type_12_7) {
    const std::string src = mainWith(
        "    var f: (Animal) -> () := lambda(a: Animal) => (); "
        "    var g: (Dog) -> () := f; "
        "    var o: Object := g; "
        "    Console.WriteLine(o is (Animal) -> ()); "
        "    Console.WriteLine(o is (Dog) -> ()); "
        "    Console.WriteLine(o is not (Dog) -> ()); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nTrue\n"));
}

// §12.7: "the target's where inferred" — a lambda whose parameter type is
// inferred from its `(Dog) -> ()` target is formed AT `(Dog) -> ()`, the
// mirror of the example above.
BEZEL_TEST(function_type_test_identity_spec_12_7_5826,
          inferred_lambda_is_formed_at_its_target_type_12_7) {
    const std::string src = mainWith(
        "    var g: (Dog) -> () := lambda(a) => (); "
        "    var o: Object := g; "
        "    Console.WriteLine(o is (Dog) -> ()); "
        "    Console.WriteLine(o is (Animal) -> ()); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\n"));
}

// §12.7 / §27.1 identity: "slot types, modifiers and return type". One line
// per value: the test at its own type, then at a type differing in exactly
// one component (a slot type, the return type, a `ref` modifier, a slot's
// nullability, the arity).
BEZEL_TEST(function_type_test_identity_spec_12_7_5826,
          identity_is_slot_types_modifiers_and_return_type_12_7) {
    const std::string src = mainWith(
        "    using var acc := new Acc(3); "
        "    var t: (int) -> int := Util.Twice; "
        "    var ot: Object := t; "
        "    Console.WriteLine($\"{ot is (int) -> int} {ot is (string) -> int} "
        "{ot is (int) -> long} {ot is (int, int) -> int} {ot is () -> int}\"); "
        "    var i: (int) -> int := acc.Add; "
        "    var oi: Object := i; "
        "    Console.WriteLine($\"{oi is (int) -> int} {oi is (int) -> long}\"); "
        "    var b: (ref int) -> () := Util.Bump; "
        "    var ob: Object := b; "
        "    Console.WriteLine($\"{ob is (ref int) -> ()} {ob is (int) -> ()}\"); "
        "    var k: (int) -> () := Util.Take; "
        "    var ok: Object := k; "
        "    Console.WriteLine($\"{ok is (int) -> ()} {ok is (ref int) -> ()}\"); "
        "    var m: (string?) -> int := Util.MaybeLen; "
        "    var om: Object := m; "
        "    Console.WriteLine($\"{om is (string?) -> int} "
        "{om is (string) -> int}\"); "
        "    var l: (string) -> int := Util.Len; "
        "    var ol: Object := l; "
        "    Console.WriteLine($\"{ol is (string) -> int} "
        "{ol is (string?) -> int}\"); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True False False False False\n"
                                "True False\n"
                                "True False\n"
                                "True False\n"
                                "True False\n"
                                "True False\n"));
}

// §12.7: "it succeeds exactly when the operand holds a function value" — a
// string, a class instance, a boxed value and null are instances of no
// function type; `as` yields null for each.
BEZEL_TEST(function_type_test_identity_spec_12_7_5826,
          non_function_operands_and_null_never_match_12_7) {
    const std::string src = mainWith(
        "    using var acc := new Acc(3); "
        "    var t: (int) -> int := Util.Twice; "
        "    Console.WriteLine($\"{Util.IsIntToInt(t)} {Util.IsIntToInt(\"s\")} "
        "{Util.IsIntToInt(5)} {Util.IsIntToInt(acc)}\"); "
        "    var s: Object := \"s\"; "
        "    var n: Object? := null; "
        "    Console.WriteLine($\"{(s as (int) -> int) = null} "
        "{n is (int) -> int} {(n as (int) -> int) = null}\"); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True False False False\nTrue False True\n"));
}

// §12.7: "`as` returns the value cast to the target type, or `null` if the
// cast fails." The successful cast is the function itself: it is invocable
// and equal (§27.7.1) to the value it came from.
BEZEL_TEST(function_type_test_identity_spec_12_7_5826,
          as_yields_the_invocable_function_or_null_12_7) {
    const std::string src = mainWith(
        "    var t: (int) -> int := Util.Twice; "
        "    var o: Object := t; "
        "    var h := o as (int) -> int; "
        "    var miss := o as (string) -> int; "
        "    if h <> null then "
        "      Console.WriteLine(h!(21)); "
        "      Console.WriteLine(h = t); "
        "    end; "
        "    Console.WriteLine(miss = null); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\nTrue\nTrue\n"));
}

// §12.7 *Value-typed operands*: "A test in which a **type parameter** stands
// on either side … is decided **per instantiation**" — the issue's `o as T`
// at a function-typed `T`.
BEZEL_TEST(function_type_test_identity_spec_12_7_5826,
          type_parameter_target_is_decided_per_instantiation_12_7) {
    const std::string src = mainWith(
        "    var t: (int) -> int := Util.Twice; "
        "    var o: Object := t; "
        "    Console.WriteLine($\"{Util.Is<(int) -> int>(o)} "
        "{Util.Is<(string) -> int>(o)} {Util.Is<string>(o)}\"); "
        "    Console.WriteLine($\"{Util.Casts<(int) -> int>(o)} "
        "{Util.Casts<(string) -> int>(o)}\"); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True False False\nTrue False\n"));
}

// §12.7 under the generated-code sanitizer, looped: `as` to the formed type
// hands back the function-typed reference for an `Object` view and the round
// trip `f as Object` -> `as (int) -> int` returns `f` itself; neither adds
// nor drops a reference.
BEZEL_TEST(function_type_test_identity_spec_12_7_5826,
          as_round_trip_is_sanitizer_clean_12_7) {
    const std::string src = mainWith(
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 100 do "
        "      var o1: Object := Util.Twice; "
        "      var o2: Object := lambda(x: int) => x + 1; "
        "      var h1 := o1 as (int) -> int; "
        "      var h2 := o2 as (int) -> int; "
        "      var h3 := o2 as (string) -> int; "
        "      if h1 <> null and h2 <> null and h3 = null then "
        "        total := total + h1!(1) + h2!(1); "
        "      end; "
        "      if o1 is (int) -> int and o2 is not (int) -> long then "
        "        total := total + 1; "
        "      end; "
        "      var f: (int) -> int := lambda(x: int) => x + i; "
        "      var q := f as Object; "
        "      if q <> null then "
        "        var back := q! as (int) -> int; "
        "        if back <> null then total := total + back!(0) - i; end; "
        "        if back = f then total := total + 1; end; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(total); ");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);                  // ASan/LSan-clean run, not a skip
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("600\n"));   // (2+2+1+0+1) * 100
}

// §12.7 / §2.8.3 rule 3: a slot type's identity is its DECLARING path, not
// its simple name. Two nested types both called `Item` are different slot
// types, so `(A.Item) -> ()` and `(B.Item) -> ()` are different function
// types (the identity key spells the enclosing-type path).
BEZEL_TEST(function_type_test_identity_spec_12_7_5826,
          same_named_nested_slot_types_are_distinct_12_7) {
    const std::string src =
        "module M5826N; "
        "class A "
        "  public class Item "
        "    public constructor() end constructor; "
        "  end Item; "
        "  public static method Take(i: Item) end Take; "
        "end A; "
        "class B "
        "  public class Item "
        "    public constructor() end constructor; "
        "  end Item; "
        "  public static method Take(i: Item) end Take; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var fa: (A.Item) -> () := A.Take; "
        "    var fb: (B.Item) -> () := B.Take; "
        "    var oa: Object := fa; "
        "    var ob: Object := fb; "
        "    Console.WriteLine($\"{oa is (A.Item) -> ()} {oa is (B.Item) -> ()} "
        "{ob is (B.Item) -> ()} {ob is (A.Item) -> ()}\"); "
        "    var back := oa as (B.Item) -> (); "
        "    Console.WriteLine(back = null); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True False True False\nTrue\n"));
}

// §12.7: "the target's where inferred" — at every position a target reaches
// a lambda, not only a typed local. A lambda with an UN-ANNOTATED parameter
// passed where a generic parameter `T` is bound to `(string) -> int` by an
// explicit type argument is formed at `(string) -> int`; the same lambda at
// a function-typed parameter likewise. (Sema records the callee's
// substituted formal as the lambda's type, #6107; the value is stamped with
// the type sema recorded.)
BEZEL_TEST(function_type_test_identity_spec_12_7_5826,
          untyped_lambda_at_generic_argument_is_formed_at_its_target_12_7) {
    const std::string src = std::string(kDecls) +
        "public static class G "
        "  public static method Pass<T>(v: T): Object => v; "
        "  public static method TakeFn(f: (string) -> int): bool "
        "    => f is (string) -> int; "
        "  public static method K(o: Object): string "
        "    => $\"{o is (string) -> int}/{o is () -> int}/"
        "{o is () -> ()}/{o is (int) -> int}\"; "
        "end G; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(G.K(G.Pass<(string) -> int>(lambda(x) => 3))); "
        "    Console.WriteLine(G.K(G.Pass<(string) -> int>("
        "lambda(x: string) => 3))); "
        "    Console.WriteLine(G.TakeFn(lambda(x) => 3)); "
        "    var p: Object := G.Pass<(string) -> int>(lambda(x) => x.Length); "
        "    Console.WriteLine(G.K(p)); "
        "    var q := p as (string) -> int; "
        "    if q <> null then Console.WriteLine(q(\"abcd\")); "
        "    else Console.WriteLine(\"as=null\"); end; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True/False/False/False\n"
                                "True/False/False/False\n"
                                "True\n"
                                "True/False/False/False\n"
                                "4\n"));
}

// §27.3 (v0.338), "Which block-bodied lambdas return a value": a block body
// with no value-carrying `return` is a procedure, so `lambda() => do end` is
// formed at `() -> ()` — as a typed local, at an `Object` target, at a
// generic argument, and un-annotated. (Sema makes the classification,
// #6107; the value is stamped with the type sema recorded and codegen
// re-derives nothing from the body.)
BEZEL_TEST(function_type_test_identity_spec_12_7_5826,
          procedure_block_lambda_is_formed_at_unit_return_27_3) {
    const std::string src = std::string(kDecls) +
        "public static class G "
        "  public static method Pass<T>(v: T): Object => v; "
        "  public static method K(o: Object): string "
        "    => $\"{o is () -> ()}/{o is () -> int}\"; "
        "end G; "
        "public class Program "
        "  public static method Main() "
        "    var u: () -> () := lambda() => do end; "
        "    var uo: Object := u; "
        "    Console.WriteLine(G.K(uo)); "
        "    var r: Object := lambda() => do end; "
        "    Console.WriteLine(G.K(r)); "
        "    Console.WriteLine(G.K(G.Pass<() -> ()>(lambda() => do end))); "
        "    var w := lambda() => do end; "
        "    var wo: Object := w; "
        "    Console.WriteLine(G.K(wo)); "
        "    var v: () -> int := lambda() => do return 3; end; "
        "    var vo: Object := v; "
        "    Console.WriteLine(G.K(vo)); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True/False\nTrue/False\nTrue/False\n"
                                "True/False\nFalse/True\n"));
}
