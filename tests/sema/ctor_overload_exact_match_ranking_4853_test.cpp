// fix #4853 (§22.6 "Implicit conversions and tie-breaking", v0.307): "one
// candidate is better than another when the better candidate requires no
// implicit conversion (the argument's static type matches the parameter type
// exactly) and the other requires an implicit conversion"; and a candidate is
// applicable only through the §5.1 implicit conversions, which never include a
// run-time narrowing (`long` → `int`, `double` → `float`).
//
// Codegen's `ctorForArgs` (src/codegen/LlvmBackend_impl.hpp) re-derived
// `new` overload resolution with a type-class proxy (bool / integral /
// floating / named / string), so the FIRST declared candidate in the
// argument's class won, exact or not, applicable or not. Sema resolved the
// same overload set correctly and discarded the answer. The fix publishes
// sema's §22.6 winner (`SemaContext::setResolvedNewCtor`) and has
// `ctorForArgs` lower it; for the call shapes sema does not resolve (a class
// with a §22.10 primary constructor, a `this(...)` chain) the proxy gains two
// leading tiers — exact identity, then no-narrowing — ahead of its old pass.
//
// Rows 1–6 are the issue's probes q1/q2/q3/q4/q6/q7
// (/var/tmp/bezel-scratch/r20/rv-l1c/p/q*.bzl); row 7 is #4873's repro, the
// same defect with class-typed parameters. Every row is a VALUE assertion on
// which constructor ran: the pre-fix defect compiles and exits 0 with the
// wrong tag (and, for q7, a truncated number), so exit status proves nothing.
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Shared hierarchy for the class-typed rows: Dog converts to Animal (§5.1
// class-to-base), Animal does not convert to Dog.
const char* kAnimals =
    "public class Animal "
    "  public constructor() end constructor; "
    "end Animal; "
    "public class Dog : Animal "
    "  public constructor() base(); end constructor; "
    "end Dog; ";

std::string program(const std::string& classC, const std::string& mainBody) {
    return std::string("module M; ") + kAnimals + classC +
           "public class Program "
           "  public static method Main(): int " +
           mainBody +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

} // namespace

// q1: `(a: Animal)` declared before `(d: Dog)`; a `Dog` argument is exact for
// `(d: Dog)` and converting for `(a: Animal)`. Pre-fix: `A`.
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           exact_class_param_beats_base_class_param_spec_22_6_q1) {
    auto r = runAot(program(
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(a: Animal) Tag := \"A\"; end constructor; "
        "  public constructor(d: Dog) Tag := \"D\"; end constructor; "
        "end C; ",
        "    var d := new Dog(); Console.WriteLine(new C(d).Tag); "
        "    var a := new Animal(); Console.WriteLine(new C(a).Tag); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "D\nA\n");
}

// q2: `(d: Dog, k := 0)` beside `(a: Animal)`. Exact-match ranking (§22.6
// stage 5) runs BEFORE the defaulted-arity tie-break (stage 6), so the exact
// candidate wins even though it fills a default. Pre-fix: `A`.
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           exact_defaulted_beats_converting_nondefaulted_spec_22_6_q2) {
    auto r = runAot(program(
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(d: Dog, k: int := 0) Tag := $\"D{k}\"; end constructor; "
        "  public constructor(a: Animal) Tag := \"A\"; end constructor; "
        "end C; ",
        "    var d := new Dog(); Console.WriteLine(new C(d).Tag); "
        "    var a := new Animal(); Console.WriteLine(new C(a).Tag); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "D0\nA\n");
}

// q3: `(n: long)` declared before `(n: int)`; the literal `5` is `int`, exact
// for `(n: int)`. Pre-fix: `L`.
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           exact_int_beats_widening_long_spec_22_6_q3) {
    auto r = runAot(program(
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(n: long) Tag := $\"L{n}\"; end constructor; "
        "  public constructor(n: int) Tag := $\"I{n}\"; end constructor; "
        "end C; ",
        "    Console.WriteLine(new C(5).Tag); "
        "    var big: long := 5000000000; Console.WriteLine(new C(big).Tag); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "I5\nL5000000000\n");
}

// q4: `(n: long)` beside `(n: int, m := 0)`: exact-match ranking before the
// defaulted-arity tie-break. Pre-fix: `L`.
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           exact_defaulted_int_beats_widening_long_spec_22_6_q4) {
    auto r = runAot(program(
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(n: long) Tag := $\"L{n}\"; end constructor; "
        "  public constructor(n: int, m: int := 0) Tag := $\"I{n}\"; end constructor; "
        "end C; ",
        "    Console.WriteLine(new C(5).Tag); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "I5\n");
}

// q6: `(f: float)` beside `(d: double, k := 0)`; `2.5` is `double`. `double`
// → `float` is not a §5.1 conversion, so `(f: float)` is not even applicable.
// Pre-fix: `F` (a silent narrowing into the wrong constructor).
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           double_arg_never_narrows_into_float_ctor_spec_22_6_5_1_q6) {
    auto r = runAot(program(
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(f: float) Tag := \"F\"; end constructor; "
        "  public constructor(d: double, k: int := 0) Tag := \"D\"; end constructor; "
        "end C; ",
        "    Console.WriteLine(new C(2.5).Tag); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "D\n");
}

// q7, the issue's headline repro: `(n: int)` beside `(l: long, k := 0)` with
// a run-time `long` of 5000000000. `long` → `int` is not a §5.1 conversion.
// Pre-fix: `I705032704` — the `int` constructor ran on a truncated value.
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           long_arg_never_truncates_into_int_ctor_spec_22_6_5_1_q7) {
    auto r = runAot(program(
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(n: int) Tag := $\"I{n}\"; end constructor; "
        "  public constructor(l: long, k: int := 0) Tag := $\"L{l}\"; end constructor; "
        "end C; ",
        "    var big: long := 5000000000; Console.WriteLine(new C(big).Tag); "
        "    Console.WriteLine(new C(7).Tag); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "L5000000000\nI7\n");
}

// #4873: `(c: Ci)` beside `(s: Sq, k := 2)`; an `Sq` argument makes
// `(c: Ci)` inapplicable (no §5.1 conversion between unrelated classes), so
// `(s: Sq, k := 2)` is the only candidate. Pre-fix: `Ci` (the Sq object was
// passed to the Ci constructor).
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           unrelated_class_param_is_inapplicable_spec_22_6_issue_4873) {
    auto r = runAot(
        "module M; "
        "public class Sq "
        "  public constructor() end constructor; "
        "end Sq; "
        "public class Ci "
        "  public constructor() end constructor; "
        "end Ci; "
        "public class H "
        "  public V: string; "
        "  public constructor(c: Ci) V := \"Ci\"; end constructor; "
        "  public constructor(s: Sq, k: int := 2) V := $\"Sq{k}\"; end constructor; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var q := new Sq(); "
        "    var c := new Ci(); "
        "    Console.WriteLine(new H(q).V); "
        "    Console.WriteLine(new H(c).V); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Sq2\nCi\n");
}

// A class WITH a §22.10 primary constructor: sema defers this overload set to
// codegen, so the choice is `ctorForArgs`'s own search — its new `Exact`
// tier. `(n: int)` is declared before `(l: long)`; a `long` argument is exact
// for `(l: long)` and `(n: int)` is inapplicable. Pre-fix: `I705032704`.
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           primary_class_exact_explicit_ctor_wins_spec_22_6_exact_tier) {
    auto r = runAot(program(
        "public class C(tag: string) "
        "  public constructor(n: int) this($\"I{n}\"); end constructor; "
        "  public constructor(l: long) this($\"L{l}\"); end constructor; "
        "  public method Get(): string return tag; end Get; "
        "end C; ",
        "    var big: long := 5000000000; Console.WriteLine(new C(big).Get()); "
        "    Console.WriteLine(new C(7).Get()); "
        "    Console.WriteLine(new C(\"p\").Get()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "L5000000000\nI7\np\n");
}

// Same deferred shape with NO exact candidate: `(n: int)` and `(d: double)`
// for a `long` argument. `long` → `double` is a §5.1 widening, `long` → `int`
// is not a conversion at all, so `(d: double)` is the only applicable
// candidate — the `NoNarrow` tier. Pre-fix: `I` (truncating).
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           primary_class_widening_beats_narrowing_spec_5_1_nonarrow_tier) {
    auto r = runAot(program(
        "public class C(tag: string) "
        "  public constructor(n: int) this(\"I\"); end constructor; "
        "  public constructor(d: double) this(\"D\"); end constructor; "
        "  public method Get(): string return tag; end Get; "
        "end C; ",
        "    var big: long := 5000000000; Console.WriteLine(new C(big).Get()); "
        "    Console.WriteLine(new C(7).Get()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "D\nI\n");
}

// A `this(...)` chain call resolves through the same `ctorForArgs` search
// (sema does not resolve a chain target). `(n: long)` is declared before
// `(n: int)`; `this(5)` is exact for `(n: int)`. Pre-fix: `L5`.
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           this_chain_exact_int_beats_widening_long_spec_22_6_exact_tier) {
    auto r = runAot(program(
        "public class C "
        "  public Tag: string; "
        "  public constructor(n: long) Tag := $\"L{n}\"; end constructor; "
        "  public constructor(n: int) Tag := $\"I{n}\"; end constructor; "
        "  public constructor(s: string) this(5); end constructor; "
        "end C; ",
        "    Console.WriteLine(new C(\"x\").Tag); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "I5\n");
}

// Combined-form record (§9.3): the auto-generated primary `(x: int)` sits
// beside an explicit `(v: int?)`. A bare `5` is exact for the primary and
// converting for `(v: int?)` (`int` → `int?` is a §5.1 nullable conversion),
// so §22.6 stage 5 selects the PRIMARY: `x = 5`. Pre-fix the type-class proxy
// treated `int`/`int?` as one class and ran the explicit ctor: `x = 50`. This
// is the shape tests/codegen/combined_record_secondary_ctor_arity_4039_test.cpp
// row T2 used to pin at 50 (that row now passes an `int?` local, for which the
// explicit ctor is the exact candidate and still yields 50).
BEZEL_TEST(sema_ctor_overload_exact_match_ranking_4853,
           combined_record_primary_exact_beats_nullable_explicit_spec_22_6_exact_tier) {
    auto r = runAot(
        "module M; record Combo4853(x: int) "
        "  public constructor(v: int?) this.x := (v ?? 0) * 10; end constructor; "
        "end Combo4853; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new Combo4853(5); Console.WriteLine(a.x); "
        "    var n: int? := 5; var b := new Combo4853(n); Console.WriteLine(b.x); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n50\n");
}
