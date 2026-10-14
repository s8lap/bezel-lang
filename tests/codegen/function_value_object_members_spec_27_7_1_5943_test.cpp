// Regression pins for issue #5943 (spec §27.7.1 "Equality of function
// values", §8.1 "Definition" of `Object`, §8.3; all v0.338): `Object`'s members
// called directly on a FUNCTION-typed receiver — `f.GetHashCode()`,
// `f.Equals(g)`, `f.ToString()`.
//
// Before the fix no call arm knew the receiver kind, so the call did not
// lower: `var h: int := f.GetHashCode();` compiled behind the `internal:`
// #2434 warning with `h` zero-initialised, and a `Console.WriteLine(
// f.Equals(g))` emitted nothing at all. Every row prints a value the
// pre-fix compiler gets wrong or omits.
//
// §27.7.1: a static method reference equals every reference to the same
// method; a class-receiver reference equals one over the same receiver; a
// lambda equals only itself; and the hash agrees with that equality.
//
// ORACLES. A function-typed local used as a receiver or argument is not
// reclaimed at scope end, before and after this fix (same allocation counts;
// the #5967 family), so the value rows run on `runAot`.
// `fresh_receiver_and_argument_are_released_27_7_1` runs looped on
// `runAotChecked` over the shape this fix newly lowers AND must reclaim: a
// receiver or argument that is itself a forming expression.
//
// `ToString()` has no §8.5 bullet for a function value; rows assert only a
// non-empty result.

#include <string>

#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kDecls =
    "module M5943; "
    "class Acc "
    "  public _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method Add(x: int): int => x + _n; "
    "end Acc; "
    "static class Demo "
    "  public static method Twice(n: int): int => n * 2; "
    "  public static method Thrice(n: int): int => n * 3; "
    "  public static method SameHash<T>(a: T, b: T): bool "
    "    => a.GetHashCode() = b.GetHashCode(); "
    "end Demo; ";

}  // namespace

// The issue's own repro, made observable: `h` is the hash, not the #2434
// zero, and agrees with the hash of an equal function value.
BEZEL_TEST(function_value_object_members_spec_27_7_1_5943,
          issue_repro_gethashcode_local_initializer_lowers_27_7_1) {
    const std::string src = std::string(kDecls) +
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> int := Demo.Twice; "
        "    var g: (int) -> int := Demo.Twice; "
        "    var k: (int) -> int := Demo.Thrice; "
        "    var h: int := f.GetHashCode(); "
        "    Console.WriteLine(h = g.GetHashCode()); "
        "    Console.WriteLine(h <> 0 or k.GetHashCode() <> 0); "
        "    Console.WriteLine(h <> k.GetHashCode()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\nTrue\n"));
}

// §27.7.1's three equality kinds through `Equals`, each with its hash. The
// class-receiver pair is compared with `=` at its function type: `a1.Equals(a2)`
// would place a receiver-bearing reference at `Equals(Object)`'s argument,
// invalid under §28.3 *Set-erasing positions* (v0.338; #6261, #6215).
BEZEL_TEST(function_value_object_members_spec_27_7_1_5943,
          equals_and_hash_follow_function_value_equality_27_7_1) {
    const std::string src = std::string(kDecls) +
        "class C "
        "  public static method Main(): int "
        "    using var acc := new Acc(1); "
        "    using var acc2 := new Acc(1); "
        "    var f: (int) -> int := Demo.Twice; "
        "    var g: (int) -> int := Demo.Twice; "
        "    var k: (int) -> int := Demo.Thrice; "
        "    Console.WriteLine($\"{f.Equals(g)} {f.Equals(k)} "
        "{f.GetHashCode() = g.GetHashCode()}\"); "
        "    var a1: (int) -> int := acc.Add; "
        "    var a2: (int) -> int := acc.Add; "
        "    var a3: (int) -> int := acc2.Add; "
        "    Console.WriteLine($\"{a1 = a2} {a1 = a3} "
        "{a1.GetHashCode() = a2.GetHashCode()}\"); "
        "    var l: (int) -> int := lambda(x: int) => x; "
        "    var l2: (int) -> int := lambda(x: int) => x; "
        "    Console.WriteLine($\"{l.Equals(l)} {l.Equals(l2)} {l.Equals(f)} "
        "{l.GetHashCode() = l.GetHashCode()}\"); "
        "    var o: Object := g; "
        "    Console.WriteLine($\"{f.Equals(o)} {f.Equals(\"Twice\")} "
        "{f.Equals(5)} {f.ToString().Length > 0}\"); "
        "    Console.WriteLine(Demo.SameHash<(int) -> int>(f, g)); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True False True\n"
                                "True False True\n"
                                "True False False True\n"
                                "True False False True\n"
                                "True\n"));
}

// A receiver or argument that is itself a forming expression is a fresh
// function object nothing binds; the member call must release it. Looped
// under the generated-code sanitizer (a missing release is 100+ leaked
// cells; a doubled one is an over-release abort).
BEZEL_TEST(function_value_object_members_spec_27_7_1_5943,
          fresh_receiver_and_argument_are_released_27_7_1) {
    const std::string src = std::string(kDecls) +
        "class C "
        "  public static method Main(): int "
        "    var good: int := 0; "
        "    var i: int := 0; "
        "    while i < 100 do "
        "      var h: int := Demo.Twice.GetHashCode(); "
        "      if h = Demo.Twice.GetHashCode() then good := good + 1; end; "
        "      if Demo.Twice.Equals(Demo.Twice) then good := good + 1; end; "
        "      if not Demo.Twice.Equals(Demo.Thrice) then "
        "        good := good + 1; "
        "      end; "
        "      if Demo.Twice.ToString().Length > 0 then good := good + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(good); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);                  // ASan/LSan-clean run, not a skip
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("400\n"));
}

// A CAPTURING lambda as the receiver or the argument is a temporary too: its
// cell AND its captured cell are reclaimed (the lambda is classed
// non-escaping exactly as one in a call-bounded argument position is,
// #3778). Before the review fix each call leaked both, 100 iterations ->
// 200+ blocks under LSan. The helpers are block-bodied and the calls sit in
// plain statements, with every function value a lambda or a local. Two
// shapes still leak and are #6108's temporary-lifetime defect, not this arm's:
//   * a capturing lambda temporary in an expression-bodied method or an `if`
//     condition leaks its captured cell — measured on the IIFE too, on the
//     base compiler: `if (lambda(x: int) => x + i)(1) > 0 then` leaks one
//     block per evaluation there. (The base did not LOWER these member calls
//     at all, so it has no leak of this exact shape to compare with.)
//   * a function value RETURNED BY A CALL is never released, in any position:
//     `if Mk(i) = Mk(i) then` leaks the same six blocks per evaluation on the
//     base as `Mk(i).Equals(Mk(i))` does here.
BEZEL_TEST(function_value_object_members_spec_27_7_1_5943,
          capturing_lambda_receiver_and_argument_are_released_27_7_1) {
    const std::string src = std::string(kDecls) +
        "static class Cap "
        "  public static method Hash(i: int): int "
        "    var h: int := (lambda(x: int) => x + i).GetHashCode(); "
        "    return h; "
        "  end Hash; "
        "  public static method SelfEq(i: int): bool "
        "    var b: bool := (lambda(x: int) => x + i).Equals("
        "lambda(x: int) => x + i); "
        "    return b; "
        "  end SelfEq; "
        "  public static method ArgEq(i: int): bool "
        "    var b: bool := Demo.Twice.Equals(lambda(x: int) => x + i); "
        "    return b; "
        "  end ArgEq; "
        "  public static method Str(i: int): int "
        "    var s: string := (lambda(x: int) => x + i).ToString(); "
        "    return s.Length; "
        "  end Str; "
        "end Cap; "
        "class C "
        "  public static method Main(): int "
        "    var good: int := 0; "
        "    var i: int := 0; "
        "    while i < 100 do "
        "      var h: int := Cap.Hash(i); "
        "      good := good + h * 0 + 1; "
        "      var a: bool := Cap.SelfEq(i); "
        "      var b: bool := Cap.ArgEq(i); "
        "      if not a and not b then good := good + 1; end; "
        "      if Cap.Str(i) > 0 then good := good + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(good); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);                  // ASan/LSan-clean run, not a skip
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

// §18.5 / §8.5 (v0.338): a function value in an interpolation hole and as
// `Console.WriteLine`'s argument renders through its `ToString` — the same
// text `f.ToString()` gives. Before the review fix the hole rendered nothing
// (`[]`, behind the `internal:` #1386 warning) and `Console.WriteLine(f)`
// passed the cell to the string printer. A hole or argument that is itself a
// forming expression is released once rendered (looped, LSan).
BEZEL_TEST(function_value_object_members_spec_27_7_1_5943,
          interpolation_hole_and_console_render_tostring_18_5) {
    const std::string src = std::string(kDecls) +
        "class C "
        "  public static method Main(): int "
        "    var good: int := 0; "
        "    var i: int := 0; "
        "    while i < 100 do "
        "      var s: string := $\"[{Demo.Twice}]\"; "
        "      var t: string := Demo.Twice.ToString(); "
        "      if s.Length = t.Length + 2 and s.Contains(t) and "
        "         t.Length > 0 then "
        "        good := good + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(good); "
        "    Console.WriteLine(Demo.Twice); "
        "    Console.WriteLine(Demo.Twice.ToString()); "
        "    Console.WriteLine($\"<{Demo.Thrice}>\".Length > 2); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);                  // ASan/LSan-clean run, not a skip
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    const std::string& out = r.stdoutText;
    BEZEL_EXPECT_TRUE(out.rfind("100\n", 0) == 0);
    // Lines 2 and 3 — `Console.WriteLine(f)` and `f.ToString()` — agree.
    const std::size_t l2 = 4;
    const std::size_t e2 = out.find('\n', l2);
    BEZEL_EXPECT_TRUE(e2 != std::string::npos && e2 > l2);
    if (e2 != std::string::npos) {
        const std::string line = out.substr(l2, e2 - l2 + 1);
        BEZEL_EXPECT_EQ(out, std::string("100\n") + line + line + "True\n");
    }
}
