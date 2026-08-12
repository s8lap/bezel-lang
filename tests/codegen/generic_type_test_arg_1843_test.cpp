// #1843 (§12.7 / §12.9 / §19.6, v0.196) — a runtime `is`/`as`/case type test
// against a CONSTRUCTED generic type must honor the type ARGUMENT. Before this
// fix every monomorphization of a generic type shared ONE runtime descriptor
// (keyed by the base declaration), so `x is Box<int>` matched a `Box<string>`
// (type confusion) and `x as Box<int>` reinterpreted the string reference as an
// int. Class hierarchies are invariant (§12.9), and §19.6 (v0.196) defines the
// exhaustiveness proof as "exactly the runtime test's static image" — so a value
// of static type `Box<string>` is NOT a `Box<int>`, and a `Tagged<int>` arm must
// not match a `Tagged<string>`.
//
// Each test observes a bool via the process exit code (1 = true, 0 = false).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Box<U> plus the sealed Result<T>/Ok<T>/Err<T> hierarchy (a generic base),
// followed by a Main body. Reused across cases.
int runProg(const std::string& mainBody) {
    return run(
        "module M; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> public Value: T; "
        "  public constructor(v: T) Value := v; end constructor; end Ok; "
        "sealed class Err<T> : Result<T> public Msg: string; "
        "  public constructor(m: string) Msg := m; end constructor; end Err; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

} // namespace

// The exact reported defect: `Box<string> is Box<int>` was True (type
// confusion). It must be False — the type argument differs.
BEZEL_TEST(codegen_1843, is_constructed_generic_wrong_arg_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var b: Box<string> := new Box<string>(\"hi\"); "
        "var o: Object := b; "
        "return (if o is Box<int> then 1 else 0);"), 0);
}

// The genuinely-matching case still matches: `Box<string> is Box<string>`.
BEZEL_TEST(codegen_1843, is_constructed_generic_right_arg_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var b: Box<string> := new Box<string>(\"hi\"); "
        "var o: Object := b; "
        "return (if o is Box<string> then 1 else 0);"), 1);
}

// `as` to the wrong instantiation yields null (§17.4) — no reinterpretation.
BEZEL_TEST(codegen_1843, as_constructed_generic_wrong_arg_is_null) {
    BEZEL_EXPECT_EQ(runProg(
        "using var b: Box<string> := new Box<string>(\"hi\"); "
        "var o: Object := b; "
        "var w: Box<int>? := o as Box<int>; "
        "return (if w = null then 1 else 0);"), 1);
}

// `as` to the correct instantiation succeeds (non-null borrow).
BEZEL_TEST(codegen_1843, as_constructed_generic_right_arg_non_null) {
    BEZEL_EXPECT_EQ(runProg(
        "using var b: Box<string> := new Box<string>(\"hi\"); "
        "var o: Object := b; "
        "var w: Box<string>? := o as Box<string>; "
        "return (if w <> null then 1 else 0);"), 1);
}

// Subtyping over a GENERIC base: Ok<int> : Result<int>, so `is Result<int>`
// holds — the per-monomorphization base descriptor must be in Ok<int>'s chain.
BEZEL_TEST(codegen_1843, is_generic_base_matching_arg_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var ok: Ok<int> := new Ok<int>(42); "
        "var r: Object := ok; "
        "return (if r is Result<int> then 1 else 0);"), 1);
}

// But `is Result<string>` on an Ok<int> is False — the base's type argument
// differs (invariance, §12.9).
BEZEL_TEST(codegen_1843, is_generic_base_wrong_arg_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var ok: Ok<int> := new Ok<int>(42); "
        "var r: Object := ok; "
        "return (if r is Result<string> then 1 else 0);"), 0);
}

// A sibling leaf at the wrong instantiation does not match: Ok<int> is Ok<int>
// but not Ok<string> nor Err<int>.
BEZEL_TEST(codegen_1843, is_generic_sibling_and_wrong_leaf_arg) {
    // Ok<int> is Ok<int> → 1; Ok<int> is Ok<string> → 0; Ok<int> is Err<int> → 0.
    // Encode all three into one exit code: 1*4 + 0*2 + 0 = 4.
    BEZEL_EXPECT_EQ(runProg(
        "using var ok: Ok<int> := new Ok<int>(42); "
        "var r: Object := ok; "
        "var a := (if r is Ok<int> then 1 else 0); "
        "var b := (if r is Ok<string> then 1 else 0); "
        "var c := (if r is Err<int> then 1 else 0); "
        "return a * 4 + b * 2 + c;"), 4);
}

// Nested generics distinguish their inner argument: Box<Box<int>> is not
// Box<Box<string>>.
BEZEL_TEST(codegen_1843, is_nested_generic_inner_arg) {
    BEZEL_EXPECT_EQ(runProg(
        "using var bb: Box<Box<int>> := new Box<Box<int>>(new Box<int>(1)); "
        "var o: Object := bb; "
        "var m := (if o is Box<Box<int>> then 1 else 0); "
        "var n := (if o is Box<Box<string>> then 1 else 0); "
        "return m * 2 + n;"), 2);
}

// §19.6 (v0.196) — a `case` type-pattern arm `var g: Tagged<int>` matches only
// the named instantiation: a Tagged<int> scrutinee takes the arm, a
// Tagged<string> falls to `_`. This is the runtime half the exhaustiveness rule
// images (the reported "Tagged<string> matches a Tagged<int> arm" defect).
BEZEL_TEST(codegen_1843, case_generic_leaf_arm_honors_arg) {
    int code = run(
        "module M; "
        "closed class Message end Message; "
        "sealed class Text : Message public B: string; "
        "  public constructor(b: string) B := b; end constructor; end Text; "
        "sealed class Tagged<U> : Message public T: U; "
        "  public constructor(t: U) T := t; end constructor; end Tagged; "
        "class C public static method Main(): int "
        "  using var ti: Tagged<int> := new Tagged<int>(5); "
        "  using var ts: Tagged<string> := new Tagged<string>(\"x\"); "
        "  var m1: Message := ti; "
        "  var m2: Message := ts; "
        "  var a := (case m1 of var g: Tagged<int> => 1, _ => 0); "
        "  var b := (case m2 of var g: Tagged<int> => 1, _ => 0); "
        "  return a * 2 + b; "  // Tagged<int> matches (1), Tagged<string> does not (0) → 2
        " end Main; end C;");
    BEZEL_EXPECT_EQ(code, 2);
}

// Regression: a NON-generic hierarchy's is/as (the erased class-tag path) is
// unchanged — Dog is Animal / Dog / not Cat.
BEZEL_TEST(codegen_1843, nongeneric_is_as_unchanged) {
    int code = run(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class Cat : Animal end Cat; "
        "class C public static method Main(): int "
        "  using var d: Dog := new Dog(); "
        "  var a: Object := d; "
        "  var x := (if a is Animal then 1 else 0); "
        "  var y := (if a is Dog then 1 else 0); "
        "  var z := (if a is Cat then 1 else 0); "
        "  return x * 4 + y * 2 + z; "  // Animal(1) Dog(1) Cat(0) → 6
        " end Main; end C;");
    BEZEL_EXPECT_EQ(code, 6);
}

// A NESTED type-parameter target inside a monomorphized generic body:
// `o is Box<Box<T>>` with T bound to int must match a Box<Box<int>> and reject a
// Box<Box<string>>. The target's inner `T` must be resolved through the active
// substitution RECURSIVELY (not just the top-level position) to the same
// per-instantiation descriptor the `new` site minted — otherwise the test was
// keyed on the unbound `T` placeholder and returned constant-false (a
// correct→wrong regression on a legal program). §19.3 (v0.182) / §19.6 (v0.196).
BEZEL_TEST(codegen_1843, is_nested_type_param_in_generic_body) {
    int code = run(
        "module M; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "class Checker<T> "
        "  public method probe(m: Object, w: Object): int "
        "    => (if m is Box<Box<T>> then 1 else 0) * 2 "
        "     + (if w is Box<Box<T>> then 1 else 0); "
        "end Checker; "
        "class C public static method Main(): int "
        "  using var bbi: Box<Box<int>> := new Box<Box<int>>(new Box<int>(1)); "
        "  using var bbs: Box<Box<string>> := "
        "      new Box<Box<string>>(new Box<string>(\"x\")); "
        "  using var ch: Checker<int> := new Checker<int>(); "
        "  var mi: Object := bbi; "
        "  var ws: Object := bbs; "
        "  return ch.probe(mi, ws); "  // match(1)*2 + wrong(0) → 2
        " end Main; end C;");
    BEZEL_EXPECT_EQ(code, 2);
}

// The same nested-type-param resolution for a `case` typed-binding arm:
// `var g: Box<Box<T>>` (T=int) matches a Box<Box<int>>, not a Box<Box<string>>.
BEZEL_TEST(codegen_1843, case_nested_type_param_in_generic_body) {
    int code = run(
        "module M; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "class Checker<T> "
        "  public method probe(m: Object, w: Object): int "
        "    var a := (case m of var g: Box<Box<T>> => 1, _ => 0); "
        "    var b := (case w of var g: Box<Box<T>> => 1, _ => 0); "
        "    return a * 2 + b; "
        "  end probe; "
        "end Checker; "
        "class C public static method Main(): int "
        "  using var bbi: Box<Box<int>> := new Box<Box<int>>(new Box<int>(1)); "
        "  using var bbs: Box<Box<string>> := "
        "      new Box<Box<string>>(new Box<string>(\"x\")); "
        "  using var ch: Checker<int> := new Checker<int>(); "
        "  var mi: Object := bbi; "
        "  var ws: Object := bbs; "
        "  return ch.probe(mi, ws); "  // match(1)*2 + wrong(0) → 2
        " end Main; end C;");
    BEZEL_EXPECT_EQ(code, 2);
}
