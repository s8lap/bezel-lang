// §12.9/§14.2/§19.3/§31.2.1 (#2012, spec v0.220, per the #2069 ruling
// /var/tmp/bezel-scratch/impl-run-2026-07-24/rulings/misc/issue-2069.md) — the
// TYPE-VARIABLE residual of the is/as/case/catch descriptor-erasure family.
//
// A runtime type-test target that MENTIONS a type parameter (`o is Wrap<T>`, a
// `case` typed binding `var x: Wrap<T>`, a `catch` arm `MyExc<T>`) is admitted at
// the generic DECLARATION (§19.3 v0.182: a type parameter may be used as a
// pattern type, matching the SUBSTITUTED argument per instantiation). But at a
// monomorphization where T is pinned to a runtime-ERASED kind — a function type,
// or a collapse at any nesting depth — the per-monomorphization descriptor key
// (appendDescKey: `Kind::Function` -> "F" at any depth) COLLIDES with a sibling
// instantiation, so the test matched the wrong instantiation and bound a value at
// a wrong static type — the #2001/#2006 memory-unsafe type-confusion class (the
// follow-on `as` reinterprets one function signature as another).
//
// Verified live on c3ea38c (probes under /var/tmp/bezel-scratch/laneF/): p_is,
// p_nested, p_case_stmt, p_case_val, p_case_bareparam, p_catch_class ALL compiled
// (rc=0) and answered TRUE — the false positive.
//
// Per the #2069 ruling (Direction 1) this instantiation is REJECTED at
// monomorphization with an implementation-limitation diagnostic (the #1943
// device) — NOT new language semantics: the DECLARATION stays valid and every
// REIFIED instantiation (`T=int`, a literal `Wrap<int>`, a folded `Wrap<[3]int>`)
// still compiles and runs the precise #1843 answer. The reject LIFTS
// automatically once function-type identity is reified (#1939). The guard uses
// the STRICT sema oracle (typeArgRuntimeReified, hoisted), which recurses into
// declared-Named arguments — NOT descTypeArgConcrete, whose non-recursion admits
// the nested collapse (#2184); the nested test below pins that choice.
//
// FAILS WITHOUT the fix (the reject programs compiled + false-positive-matched);
// PASSES WITH (they are rejected, so runAot reports ran=false with the
// "not runtime-distinguishable" diagnostic). The ACCEPT programs compile+run.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff the program was REJECTED at compile time with THIS guard's diagnostic.
// The phrase "not runtime-distinguishable" is the shared marker of the erasure
// family; the "#2069 ruling" citation is unique to this per-monomorphization
// guard, so the two together pin the rejection to the RIGHT reason (not an
// unrelated compile error — which would make the test pass for the wrong cause).
bool rejectsWithCollapse(const std::string& src) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not runtime-distinguishable") != std::string::npos &&
            d.message.find("#2069") != std::string::npos)
            return true;
    return false;
}

// The exit code, or -999 if the program did not compile+run.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// A phantom `Wrap<W>` (W is a type argument, never stored — isolates the
// descriptor-key recursion), an `Inner<V>` for the nested case, plus a Main body.
std::string prog(const std::string& body) {
    return "module M; "
           "class Wrap<W> public tag: int; "
           "  public constructor(t: int) tag := t; end constructor; end Wrap; "
           "class Inner<V> end Inner; "
           "class Foo end Foo; "
           "class C public static method Main(): int " +
           body + " end Main; end C;";
}

}  // namespace

// ============================ REJECT: the live holes ========================

// (1) is/as headline (§19.3): `o is Wrap<T>` inside `Chk<T>` monomorphized at
// T=(int)->int, against a `Wrap<(string)->string>` value. WITHOUT the fix this
// compiled and answered TRUE (type confusion). REJECTED.
BEZEL_TEST(type_test_tvar_erased_2012, is_function_typearg_collapsed_reject_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Chk<T> public method probe(o: Object): int "
        "  => (if o is Wrap<T> then 1 else 0); end Chk; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(7); "
        "  var o: Object := w; "
        "  using var ck := new Chk<(int) -> int>(); "
        "  return ck.probe(o); end Main; end C;"));
}

// (2) `as` to a collapsed type-variable target (§17.4) is the same hole (the
// reinterpretation precursor) — REJECTED.
BEZEL_TEST(type_test_tvar_erased_2012, as_function_typearg_collapsed_reject_spec_17_4) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Chk<T> public method probe(o: Object): int "
        "  var w2: Wrap<T>? := o as Wrap<T>; "
        "  return (if w2 <> null then 1 else 0); "
        "  end probe; end Chk; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(7); "
        "  var o: Object := w; "
        "  using var ck := new Chk<(int) -> int>(); "
        "  return ck.probe(o); end Main; end C;"));
}

// (3) NESTED (§19.3, #2184): `o is Wrap<Inner<T>>` at T=(int)->int. The outer
// Named reifies, but the inner argument is a function type — the descriptor
// collapses at DEPTH. This pins the STRICT recursive oracle: descTypeArgConcrete
// returns true for a declared Named without recursing, so it would MISS this;
// typeArgRuntimeReified recurses and catches it. REJECTED.
BEZEL_TEST(type_test_tvar_erased_2012, is_nested_function_typearg_collapsed_reject_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Inner<V> end Inner; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Chk<T> public method probe(o: Object): int "
        "  => (if o is Wrap<Inner<T>> then 1 else 0); end Chk; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<Inner<(string) -> string>>(7); "
        "  var o: Object := w; "
        "  using var ck := new Chk<(int) -> int>(); "
        "  return ck.probe(o); end Main; end C;"));
}

// (4) STATEMENT case typed binding (§19.3): `case o of var x: Wrap<T> => do..end`.
// REJECTED (routes through patternMatches).
BEZEL_TEST(type_test_tvar_erased_2012, case_stmt_function_typearg_collapsed_reject_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Chk<T> public method probe(o: Object): int "
        "  var r := 0; "
        "  case o of var x: Wrap<T> => do r := 1; end, _ => do r := 0; end end; "
        "  return r; "
        "  end probe; end Chk; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(7); "
        "  var o: Object := w; "
        "  using var ck := new Chk<(int) -> int>(); "
        "  return ck.probe(o); end Main; end C;"));
}

// (5) VALUE-form case typed binding (§19.3): `=> (case o of var x: Wrap<T> => 1,
// _ => 0)`. REJECTED (the value-form CaseExpr routes through the same
// patternMatches path).
BEZEL_TEST(type_test_tvar_erased_2012, case_value_function_typearg_collapsed_reject_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Chk<T> public method probe(o: Object): int "
        "  => (case o of var x: Wrap<T> => 1, _ => 0); end Chk; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(7); "
        "  var o: Object := w; "
        "  using var ck := new Chk<(int) -> int>(); "
        "  return ck.probe(o); end Main; end C;"));
}

// (6) BARE type-parameter pattern whose SUBSTITUTED target is a collapsed
// constructed generic (§19.3): `case o of var x: T` with T=Wrap<(int)->int>. This
// exercises the emitTypeParamPatternTest surface (targetFromTypeVar). REJECTED.
BEZEL_TEST(type_test_tvar_erased_2012, case_bare_param_to_collapsed_generic_reject_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Chk<T> public method probe(o: Object): int "
        "  var r := 0; "
        "  case o of var x: T => do r := 1; end, _ => do r := 0; end end; "
        "  return r; "
        "  end probe; end Chk; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(7); "
        "  var o: Object := w; "
        "  using var ck := new Chk<Wrap<(int) -> int>>(); "
        "  return ck.probe(o); end Main; end C;"));
}

// (7) CATCH arm (§31.2.1): `catch var e: MyExc<T>` inside `Thrower<T>`
// monomorphized at T=(int)->int, with a thrown MyExc<(string)->string>. WITHOUT
// the fix the arm caught it (wrong-instantiation, wrong static type). REJECTED.
BEZEL_TEST(type_test_tvar_erased_2012, catch_arm_function_typearg_collapsed_reject_spec_31_2_1) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class MyExc<E>: Exception "
        "  public constructor(msg: string) base(msg); end constructor; end MyExc; "
        "class Thrower<T> public method go(): int "
        "  try "
        "    throw new MyExc<(string) -> string>(\"a\"); "
        "  catch "
        "    var e1: MyExc<T> => do return 1; end, "
        "    var e2: Exception => do return 2; end "
        "  end; "
        "  return 0; "
        "  end go; end Thrower; "
        "class C public static method Main(): int "
        "  using var t := new Thrower<(int) -> int>(); "
        "  return t.go(); end Main; end C;"));
}

// (8) PER-INSTANTIATION firing (§14.2): a program that instantiates the SAME
// generic at BOTH a reified argument (`Chk<int>`, sound) AND an erased one
// (`Chk<(int)->int>`, collapsed). The erased instantiation is emitted, so the
// whole compile is REJECTED — the reject is per-monomorphization, exactly as
// §14.2's per-instantiation bound/constraint checks are.
BEZEL_TEST(type_test_tvar_erased_2012, per_instantiation_firing_reject_spec_14_2) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Chk<T> public method probe(o: Object): int "
        "  => (if o is Wrap<T> then 1 else 0); end Chk; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<int>(1); "
        "  var oi: Object := wi; "
        "  using var cki := new Chk<int>(); "
        "  var a := cki.probe(oi); "
        "  using var ckf := new Chk<(int) -> int>(); "
        "  var b := ckf.probe(oi); "
        "  return a + b; end Main; end C;"));
}

// ==================== ACCEPT: reified targets stay admitted =================

// (9) The over-rejection control (§19.3, #1843): `is Wrap<T>` at T=int is
// runtime-REIFIED, so it compiles and runs the PRECISE per-instantiation answer —
// a `Wrap<int>` value matches (1), a `Wrap<string>` value does not (encoded 1*2).
// The conservative reject must NOT fire on a reified type-variable target.
BEZEL_TEST(type_test_tvar_erased_2012, is_reified_typearg_at_monomorphization_accepted_spec_19_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Chk<T> public method probe(m: Object, w: Object): int "
        "  => (if m is Wrap<T> then 1 else 0) * 2 "
        "   + (if w is Wrap<T> then 1 else 0); end Chk; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<int>(1); "
        "  using var ws := new Wrap<string>(2); "
        "  using var ck := new Chk<int>(); "
        "  var mi: Object := wi; var ws2: Object := ws; "
        "  return ck.probe(mi, ws2); end Main; end C;"), 2);
}

// (10) A LITERAL reified target at a non-generic site is unaffected: `is Wrap<int>`
// on a Wrap<int> value is true (1), on a Wrap<string> value is false — the guard
// only fires on a type-VARIABLE-mentioning target, never a literal ground one
// (sema already ruled on those; #2006/#2012/#2018).
BEZEL_TEST(type_test_tvar_erased_2012, is_reified_literal_target_accepted) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var w := new Wrap<int>(7); "
        "  var o: Object := w; "
        "  var a := (if o is Wrap<int> then 1 else 0); "
        "  var b := (if o is Wrap<string> then 1 else 0); "
        "  return a * 2 + b;")), 2);
}

// (11) The nested REIFIED analog stays admitted and precise: `is Wrap<Inner<T>>`
// at T=int matches a Wrap<Inner<int>> and not a Wrap<Inner<string>> — the guard's
// recursion must not over-reject a fully-reified nesting.
BEZEL_TEST(type_test_tvar_erased_2012, is_nested_reified_typearg_accepted_spec_19_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Inner<V> end Inner; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Chk<T> public method probe(m: Object, w: Object): int "
        "  => (if m is Wrap<Inner<T>> then 1 else 0) * 2 "
        "   + (if w is Wrap<Inner<T>> then 1 else 0); end Chk; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<Inner<int>>(1); "
        "  using var ws := new Wrap<Inner<string>>(2); "
        "  using var ck := new Chk<int>(); "
        "  var mi: Object := wi; var ms: Object := ws; "
        "  return ck.probe(mi, ms); end Main; end C;"), 2);
}

// (12) CATCH accept control (§31.2.1, the #1505 family): `catch var e: MyExc<T>`
// at T=string (a reified argument) still catches a thrown MyExc<string> (1) and
// lets a MyExc<int> fall through (2) — the held-off type-variable arm must stay
// ADMITTED for a reified instantiation. Encoded 1*10 + 2 = 12.
BEZEL_TEST(type_test_tvar_erased_2012, catch_arm_reified_typearg_accepted_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class MyExc<T>: Exception "
        "  public constructor(msg: string) base(msg); end constructor; end MyExc; "
        "static class H "
        "  public static method Probe<T>(action: () -> ()): int "
        "    try action(); "
        "    catch "
        "      var e1: MyExc<T> => do return 1; end, "
        "      var e2: Exception => do return 2; end "
        "    end; "
        "    return 0; end Probe; end H; "
        "class C public static method Main(): int "
        "  var a := H.Probe<string>(lambda() => do "
        "    throw new MyExc<string>(\"a\"); end); "
        "  var b := H.Probe<string>(lambda() => do "
        "    throw new MyExc<int>(\"b\"); end); "
        "  return a * 10 + b; end Main; end C;"), 12);
}

// (13) A NON-generic type-test target in a generic body is untouched: `is Foo`
// inside `Chk<T>` at T=(int)->int compiles and runs (Foo has no type argument to
// collapse) — proves the guard is scoped to CONSTRUCTED-generic targets only.
BEZEL_TEST(type_test_tvar_erased_2012, nongeneric_target_in_generic_body_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Foo end Foo; "
        "class Chk<T> public method probe(o: Object): int "
        "  => (if o is Foo then 1 else 0); end Chk; "
        "class C public static method Main(): int "
        "  using var f := new Foo(); "
        "  var o: Object := f; "
        "  using var ck := new Chk<(int) -> int>(); "
        "  return ck.probe(o); end Main; end C;"), 1);
}
