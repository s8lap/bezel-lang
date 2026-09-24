// fix #4790 (§22.5 "Optional parameters and named arguments" / §22.6 stage 1
// + stage 3, v0.307): codegen's constructor selector `ctorForArgs`
// (src/codegen/LlvmBackend_impl.hpp) applied its §22.6 stage-3 type-class
// check ONLY to the exact-arity arms, never to the defaulted-candidate fill
// loop. An equal-arity candidate whose NAME binds but whose parameter TYPE
// disagrees therefore either (a) short-circuited resolution with #3760's
// null before the defaulted-fill loop ever ran (single equal-arity sibling —
// silent zero-fill), or (b) — after #4779 added a "first candidate that
// binds by name" fallback for the multi-candidate arm — got selected
// OUTRIGHT over the correct defaulted sibling, producing a codegen IR
// verification failure when the wrong-typed argument was lowered into the
// wrong-typed slot.
//
// Found in §17 review of #4779's fix (0ef817e2). Both repros below are the
// issue's own probe files verbatim
// (/var/tmp/bezel-scratch/r20/review-4779/pA_eqarity_binds_typemismatch.bzl,
// pD_multi_fallback_typemismatch.bzl).
//
// Every row is a VALUE assertion on stdout (or `r.ran`, for the crash case)
// — the pre-fix Repro A failure compiles, runs, and exits 0 with an empty
// line, so `ran`/exit-code alone would prove nothing there; the pre-fix
// Repro D failure is a `bezelc: codegen error: IR verification failed`
// compile-time crash, so `r.ran` is exactly the right oracle there.
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// Repro A (pA_eqarity_binds_typemismatch.bzl): ONE equal-arity sibling
// `(x: string)` beside the defaulted `(x: int, y: int := 0)`. §22.5 binds
// `x := 5` (and positional `5`) to `(x: string)`'s own `x` by name/position,
// but §22.6 stage 3 rejects it on type (int vs string) — so §22.5 binding
// then continues to the defaulted `(x: int, y: int := 0)`, which both binds
// AND type-matches. Pre-fix: the `ctorArityCount == 1` arm's early
// `argsCanFillSymbolic(...) -> return nullptr` fired before the defaulted
// candidates were ever tried, so `new C(x := 5)`/`new C(5)` silently
// zero-filled (`Tag` printed empty); `new C(x := "q")`, which DOES both bind
// and type-match `(x: string)` at the exact-arity arm, was unaffected.
BEZEL_TEST(codegen_ctor_type_class_defaulted_sibling_4790,
           repro_a_single_equal_arity_sibling_type_mismatch_spec_22_5_22_6) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(x: string) "
        "    Tag := $\"S{x}\"; "
        "  end constructor; "
        "  public constructor(x: int, y: int := 0) "
        "    Tag := $\"XY{x},{y}\"; "
        "  end constructor; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new C(x := 5); "
        "    Console.WriteLine(a.Tag); "
        "    var b := new C(5); "
        "    Console.WriteLine(b.Tag); "
        "    var c := new C(x := \"q\"); "
        "    Console.WriteLine(c.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "XY5,0\nXY5,0\nSq\n");
}

// Repro D (pD_multi_fallback_typemismatch.bzl): TWO equal-arity siblings
// `(z: int)`, `(x: string)` plus the defaulted `(x: int, y: int := 0)`.
// `new C(x := 5)`: neither `(z: int)` (name doesn't bind) nor `(x: string)`
// (name binds, type doesn't) survives the multi-candidate arm's type check,
// so — pre-#4790 — the #4779 "first candidate that binds by name" fallback
// picked `(x: string)` and lowered the int `5` into a string slot:
// `bezelc: codegen error: IR verification failed`. Post-fix, resolution
// falls through with NO fallback to the defaulted-fill loop, which rejects
// `(x: string)` on type (the same check the exact-arity arms apply) and
// selects `(x: int, y: int := 0)`. `x := "q"` and `z := 4` are controls: each
// both binds AND type-matches an exact-arity candidate directly, so they
// exercise the (untouched) `ctorTypeMatches` success path within the same
// three-candidate class, pinning that removing the fallback didn't disturb
// the arm's normal case.
BEZEL_TEST(codegen_ctor_type_class_defaulted_sibling_4790,
           repro_d_two_equal_arity_siblings_ir_verification_spec_22_6) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(z: int) "
        "    Tag := $\"Z{z}\"; "
        "  end constructor; "
        "  public constructor(x: string) "
        "    Tag := $\"S{x}\"; "
        "  end constructor; "
        "  public constructor(x: int, y: int := 0) "
        "    Tag := $\"XY{x},{y}\"; "
        "  end constructor; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new C(x := 5); "
        "    Console.WriteLine(a.Tag); "
        "    var b := new C(x := \"q\"); "
        "    Console.WriteLine(b.Tag); "
        "    var c := new C(z := 4); "
        "    Console.WriteLine(c.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "XY5,0\nSq\nZ4\n");
}

// Loop-amplified (50 iterations): the ctor selection made at each `new`
// call site is a compile-time choice, evaluated identically every
// iteration, so a wrong selection produces a wrong SUM rather than a
// possibly-coincidentally-right one-shot value. `C` is Repro A's shape
// (single equal-arity type-mismatched sibling; pre-fix silently zero-fills,
// contributing 0 every iteration); `D` is Repro D's shape (two equal-arity
// siblings; pre-fix crashes at compile time on the very first specialization,
// so `r.ran` alone already catches it, but the value assertion also confirms
// post-fix correctness across all 50 draws of `i`).
BEZEL_TEST(codegen_ctor_type_class_defaulted_sibling_4790,
           loop_50_iterations_spec_22_5_22_6) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property V: int; "
        "  public constructor(x: string) "
        "    V := -999; "
        "  end constructor; "
        "  public constructor(x: int, y: int := 0) "
        "    V := x * 100 + y; "
        "  end constructor; "
        "end C; "
        "public class D "
        "  public readonly property V: int; "
        "  public constructor(z: int) "
        "    V := -1000 - z; "
        "  end constructor; "
        "  public constructor(x: string) "
        "    V := -2000; "
        "  end constructor; "
        "  public constructor(x: int, y: int := 0) "
        "    V := x * 100 + y; "
        "  end constructor; "
        "end D; "
        "public class Program "
        "  public static method Main(): int "
        "    var sc := 0; "
        "    var sd := 0; "
        "    for var i in 0..<50 do "
        "      var c := new C(x := i); "
        "      sc := sc + c.V; "
        "      var d := new D(x := i); "
        "      sd := sd + d.V; "
        "    end; "
        "    Console.WriteLine($\"{sc} {sd}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // sum_{i=0}^{49} i = 1225; both sc and sd = 100 * 1225 = 122500.
    BEZEL_EXPECT_EQ(r.stdoutText, "122500 122500\n");
}

// ---------------------------------------------------------------------------
// fix #4790 rework (§5.1 / §14.2 clause 5 / §22.6 stages 3 + 5, v0.307): the
// first #4790 commit made the class-comparison proxy (`typeClassOf`) an
// applicability GATE for every candidate, including the defaulted-fill loop
// that previously checked binding only. The proxy does not model §5.1: a
// primitive boxed into the builtin interface it conforms to (class 1/0 vs 3),
// or `null` into a type-parameter slot (3 vs -1), was ruled inapplicable, so
// `ctorForArgs` returned null for calls sema had accepted and the object was
// silently default-initialized (every `Tag` below printed empty). The rows are
// the §17 reviewer's probes verbatim (/var/tmp/bezel-scratch/r20/rv-4790/p/),
// each a VALUE assertion — the defect compiles and exits 0.
// ---------------------------------------------------------------------------

// p1_iface_default: the ONLY constructor takes a boxed-interface first
// parameter and a defaulted second; `5` (positional and named) converts to
// `IComparable<int>` by §14.2 clause 5 boxing.
BEZEL_TEST(codegen_ctor_type_class_defaulted_sibling_4790,
           rework_p1_boxed_iface_param_with_default_spec_5_1_14_2) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(v: IComparable<int>, k: int := 7) "
        "    Tag := $\"IC{k}\"; "
        "  end constructor; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new C(5); "
        "    Console.WriteLine(a.Tag); "
        "    var b := new C(v := 5); "
        "    Console.WriteLine(b.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "IC7\nIC7\n");
}

// p6_iface_named_multi: two equal-arity siblings; the name `v` binds only the
// boxed-interface one, which must then be applicable through boxing.
BEZEL_TEST(codegen_ctor_type_class_defaulted_sibling_4790,
           rework_p6_boxed_iface_named_beside_bool_sibling_spec_5_1_22_5) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(v: IComparable<int>) "
        "    Tag := $\"IC{v.CompareTo(3)}\"; "
        "  end constructor; "
        "  public constructor(z: bool) "
        "    Tag := \"B\"; "
        "  end constructor; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new C(v := 5); "
        "    Console.WriteLine(a.Tag); "
        "    var b := new C(z := true); "
        "    Console.WriteLine(b.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "IC1\nB\n");
}

// p8_bool_iface: `bool` boxes into `IEquatable<bool>` (stdlib §9.4).
BEZEL_TEST(codegen_ctor_type_class_defaulted_sibling_4790,
           rework_p8_bool_into_iequatable_bool_spec_5_1_14_2) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(v: IEquatable<bool>, k: int := 7) "
        "    Tag := $\"EQ{k}\"; "
        "  end constructor; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(true).Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "EQ7\n");
}

// p9_generic_null: `null` into a type-parameter slot `T` instantiated at
// `string?` (§5.1 / §7), with and without a defaulted sibling slot.
BEZEL_TEST(codegen_ctor_type_class_defaulted_sibling_4790,
           rework_p9_null_into_type_parameter_slot_spec_5_1_7) {
    auto r = runAot(
        "module M; "
        "public class Box<T> "
        "  public readonly property Tag: string; "
        "  public constructor(v: T, k: int := 9) Tag := $\"T{k}\"; end constructor; "
        "end Box; "
        "public class One<T> "
        "  public readonly property Tag: string; "
        "  public constructor(v: T) Tag := \"One\"; end constructor; "
        "end One; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new Box<string?>(null).Tag); "
        "    Console.WriteLine(new Box<string?>(v := null, k := 2).Tag); "
        "    Console.WriteLine(new One<string?>(null).Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "T9\nT2\nOne\n");
}

// Ranking controls (§22.6 stage 5 exact-match preference): the relaxed pass
// runs only when the strict one finds nothing, so an exact candidate still
// beats a boxing one whatever the declaration order, and a §22.10 PRIMARY
// constructor that matches exactly still wins over an explicit constructor
// reachable only by boxing (a named argument that binds only the explicit
// one still selects it).
BEZEL_TEST(codegen_ctor_type_class_defaulted_sibling_4790,
           rework_control_exact_beats_boxing_and_primary_spec_22_6) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(v: IComparable<int>) "
        "    Tag := \"IC\"; "
        "  end constructor; "
        "  public constructor(n: int) "
        "    Tag := $\"N{n}\"; "
        "  end constructor; "
        "end C; "
        "public class P(a: int) "
        "  public constructor(v: IComparable<int>, k: int := 7) "
        "    this(99); "
        "  end constructor; "
        "  public method Show(): string return $\"A{a}\"; end Show; "
        "end P; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(5).Tag); "
        "    Console.WriteLine(new P(5).Show()); "
        "    Console.WriteLine(new P(v := 5).Show()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "N5\nA5\nA99\n");
}
