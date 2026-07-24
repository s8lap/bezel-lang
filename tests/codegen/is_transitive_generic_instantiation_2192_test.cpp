// §14.1/§12.7/§12.9/§19.3/§31.2.1 (#2192, spec v0.226) — a generic instantiated
// TRANSITIVELY from within another generic body breaks per-instantiation
// descriptor resolution.
//
// §14.1 "monomorphization": a generic body is specialized with each type
// parameter replaced by the supplied type argument — and that replacement
// COMPOSES when the argument is itself a type parameter of an enclosing generic
// being monomorphized. For
//   class A<T> ... o is Wrap<T> ...
//   class B<U> ... new A<U>() ... a.probe(o)
//   new B<(int)->int>()   // or new B<int>()
// the receiver `a`'s type is `A<U>`, so its type argument is the still-open `U`.
// The callee substitution the method call installs was composing the LLVM side
// (lowerTypeRef reads genericSubst_, the outer map) but NOT the SEMA side
// (newSema — which drives perInstDescriptorForTarget / the #2012
// rejectCollapsedTypeTestTarget guard), leaving the callee env `T -> U` instead
// of the fully-monomorphized `T -> (int)->int` / `T -> int`. Every `A<U>` then
// collapsed onto ONE shared (erased) descriptor, so inside `probe` the runtime
// type test `o is Wrap<T>` was BOTH:
//   * false-TRUE — a `Wrap<(string)->string>` value answered `is Wrap<T>` TRUE at
//     T=(int)->int (the #2001/#2006 type-confusion class, a memory-unsafety
//     precursor via a following `as`); AND
//   * false-FALSE — a reified `Wrap<int>` value FAILED `is Wrap<T=int>` (a wrong
//     answer no reject-only guard can produce, proving the defect is in
//     descriptor resolution, not the #2012/#2069 reject guard).
//
// Verified live on base a7881f3 (the read-only main binary; probes under
// /var/tmp/bezel-scratch/laneJ/): uf6c/uf6h -> exit 7 (false-TRUE), uf6f/uf6i ->
// exit 9 (false-FALSE); the transitive-2-level, `as`, `case`, nested and `catch`
// analogs matched the same both-directions pattern.
//
// The FIX (LlvmBackend_Calls2.cpp, the method-call substitution setup) composes
// the SEMA argument through the active outer substitution
// (substituteTypeRefLocal over genericSubstSemaTy_) so the callee's descriptor
// env is the fully-monomorphized argument. Then:
//   * the false-FALSE direction becomes the PRECISE per-instantiation answer
//     (#1843/#1939): a reified `Wrap<int>` matches `is Wrap<T=int>` and a sibling
//     `Wrap<string>` does not; AND
//   * the false-TRUE direction, now seeing the collapsed ground argument
//     `T=(int)->int`, hits the #2012 per-monomorphization guard and is REJECTED
//     (the #2069-Direction-1 posture — the erased/collapsed target is not
//     runtime-distinguishable; the reject lifts when function-type identity is
//     reified, #1939). The soundness hole (a false-TRUE match, or merely
//     admitting a collapsed target) is closed either way.
//
// FAILS WITHOUT the fix: the REJECT programs compiled (false-TRUE / admitted a
// collapsed target) and the MATCH programs answered no-match. PASSES WITH: the
// REJECT programs are rejected with the erasure-guard diagnostic, the MATCH
// programs run the precise answer. The fix does not disturb the DIRECT path
// (empty outer env -> identity substitution): the #2012 direct reject and the
// #1843/#1939 direct reified-match controls stay exactly as before.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff the program was REJECTED at compile time with the erasure-family
// guard's diagnostic. The phrase "not runtime-distinguishable" is the shared
// marker; the "#2069 ruling" citation pins the rejection to the RIGHT reason (a
// per-monomorphization collapsed-target reject), not an unrelated compile error.
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

}  // namespace

// ==================== FALSE-NEGATIVE -> precise per-inst MATCH ===============
// (the wrong answer no reject-only guard can produce: a reified target that
// FAILED to match under the collapsed descriptor now matches precisely)

// (1) is / transitive-1-level / reified (§14.1, §12.7, §19.3). B<int> creates
// A<int>; inside `probe`, `m is Wrap<T=int>` must match a Wrap<int> value (1*2)
// and a sibling Wrap<string> must NOT match (0). WITHOUT the fix both were
// no-match (0). Encoded 1*2 + 0 = 2.
BEZEL_TEST(is_transitive_generic_2192, is_transitive_reified_int_matches_2192_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(m: Object, w: Object): int "
        "  => (if m is Wrap<T> then 1 else 0) * 2 "
        "   + (if w is Wrap<T> then 1 else 0); end A; "
        "class B<U> public method run(m: Object, w: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(m, w); end run; end B; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<int>(1); "
        "  using var ws := new Wrap<string>(2); "
        "  var mi: Object := wi; var wsx: Object := ws; "
        "  using var b := new B<int>(); "
        "  return b.run(mi, wsx); end Main; end C;"), 2);
}

// (2) is / transitive-2-level / reified (§14.1). D<int> creates B<int> creates
// A<int> — the composition must chain through TWO enclosing monomorphizations.
// Encoded 2 (match + no-match).
BEZEL_TEST(is_transitive_generic_2192, is_transitive2_reified_int_matches_2192_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(m: Object, w: Object): int "
        "  => (if m is Wrap<T> then 1 else 0) * 2 "
        "   + (if w is Wrap<T> then 1 else 0); end A; "
        "class B<U> public method run(m: Object, w: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(m, w); end run; end B; "
        "class D<X> public method run2(m: Object, w: Object): int "
        "  using var b := new B<X>(); "
        "  return b.run(m, w); end run2; end D; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<int>(1); "
        "  using var ws := new Wrap<string>(2); "
        "  var mi: Object := wi; var wsx: Object := ws; "
        "  using var d := new D<int>(); "
        "  return d.run2(mi, wsx); end Main; end C;"), 2);
}

// (3) as / transitive-1-level / reified (§17.4). `m as Wrap<T=int>` is non-null
// on a Wrap<int> and null on a Wrap<string>. Encoded 2. (RED: both null -> 0.)
BEZEL_TEST(is_transitive_generic_2192, as_transitive_reified_int_matches_2192_17_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(m: Object, w: Object): int "
        "  var a2: Wrap<T>? := m as Wrap<T>; "
        "  var b2: Wrap<T>? := w as Wrap<T>; "
        "  return (if a2 <> null then 1 else 0) * 2 "
        "       + (if b2 <> null then 1 else 0); "
        "  end probe; end A; "
        "class B<U> public method run(m: Object, w: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(m, w); end run; end B; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<int>(1); "
        "  using var ws := new Wrap<string>(2); "
        "  var mi: Object := wi; var wsx: Object := ws; "
        "  using var b := new B<int>(); "
        "  return b.run(mi, wsx); end Main; end C;"), 2);
}

// (4) case (statement, typed binding) / transitive-1-level / reified (§19.3).
// `case m of var x: Wrap<T=int>` binds on a Wrap<int> and not on a Wrap<string>.
// Encoded 2. (RED: neither arm bound -> 0.)
BEZEL_TEST(is_transitive_generic_2192, case_stmt_transitive_reified_int_matches_2192_19_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(m: Object, w: Object): int "
        "  var r := 0; "
        "  case m of var x: Wrap<T> => do r := r + 2; end, _ => do end end; "
        "  case w of var y: Wrap<T> => do r := r + 1; end, _ => do end end; "
        "  return r; end probe; end A; "
        "class B<U> public method run(m: Object, w: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(m, w); end run; end B; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<int>(1); "
        "  using var ws := new Wrap<string>(2); "
        "  var mi: Object := wi; var wsx: Object := ws; "
        "  using var b := new B<int>(); "
        "  return b.run(mi, wsx); end Main; end C;"), 2);
}

// (5) is / transitive-1-level / NESTED reified (§19.3, #2184 recursion). At the
// composed T=int, `is Wrap<Inner<T>>` matches a Wrap<Inner<int>> and not a
// Wrap<Inner<string>>. The composition must reach the NESTED argument. Encoded 2.
BEZEL_TEST(is_transitive_generic_2192, is_nested_transitive_reified_matches_2192_19_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Inner<V> end Inner; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(m: Object, w: Object): int "
        "  => (if m is Wrap<Inner<T>> then 1 else 0) * 2 "
        "   + (if w is Wrap<Inner<T>> then 1 else 0); end A; "
        "class B<U> public method run(m: Object, w: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(m, w); end run; end B; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<Inner<int>>(1); "
        "  using var ws := new Wrap<Inner<string>>(2); "
        "  var mi: Object := wi; var wsx: Object := ws; "
        "  using var b := new B<int>(); "
        "  return b.run(mi, wsx); end Main; end C;"), 2);
}

// (6) catch / transitive-1-level / reified (§31.2.1). Outer<string> creates
// Thrower<string>; the `catch var e1: MyExc<T=string>` arm must catch a thrown
// MyExc<string> (return 1) rather than fall through to the Exception arm (2).
// WITHOUT the fix the arm's T stayed open, the collapsed arm descriptor did not
// match, and it fell through -> 2.
BEZEL_TEST(is_transitive_generic_2192, catch_transitive_reified_matches_2192_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class MyExc<E>: Exception "
        "  public constructor(msg: string) base(msg); end constructor; end MyExc; "
        "class Thrower<T> public method go(): int "
        "  try "
        "    throw new MyExc<string>(\"a\"); "
        "  catch "
        "    var e1: MyExc<T> => do return 1; end, "
        "    var e2: Exception => do return 2; end "
        "  end; "
        "  return 0; end go; end Thrower; "
        "class Outer<U> public method run(): int "
        "  using var t := new Thrower<U>(); "
        "  return t.go(); end run; end Outer; "
        "class C public static method Main(): int "
        "  using var o := new Outer<string>(); "
        "  return o.run(); end Main; end C;"), 1);
}

// ============ FALSE-POSITIVE -> per-monomorphization REJECT ==================
// (the composed argument is a runtime-erased/collapsed kind; the #2012 guard now
// SEES it — pre-fix the callee saw the still-open outer parameter and DEFERRED,
// admitting the collapse)

// (7) is / transitive-1-level / function type (§14.1, §12.7). B<(int)->int>
// creates A<(int)->int>; `o is Wrap<T=(int)->int>` against a
// Wrap<(string)->string> value. WITHOUT the fix this compiled and answered TRUE
// (type confusion). Now REJECTED.
BEZEL_TEST(is_transitive_generic_2192, is_transitive_function_typearg_collapsed_reject_2192_14_1) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(o: Object): int "
        "  => (if o is Wrap<T> then 7 else 9); end A; "
        "class B<U> public method run(o: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(o); end run; end B; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(1); "
        "  var o: Object := w; "
        "  using var b := new B<(int) -> int>(); "
        "  return b.run(o); end Main; end C;"));
}

// (8) is / transitive-2-level / function type (§14.1). D<(int)->int> ->
// B<(int)->int> -> A<(int)->int>: the collapse must be detected after composing
// through TWO enclosing monomorphizations. REJECTED.
BEZEL_TEST(is_transitive_generic_2192, is_transitive2_function_typearg_collapsed_reject_2192_14_1) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(o: Object): int "
        "  => (if o is Wrap<T> then 7 else 9); end A; "
        "class B<U> public method run(o: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(o); end run; end B; "
        "class D<X> public method run2(o: Object): int "
        "  using var b := new B<X>(); "
        "  return b.run(o); end run2; end D; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(1); "
        "  var o: Object := w; "
        "  using var d := new D<(int) -> int>(); "
        "  return d.run2(o); end Main; end C;"));
}

// (9) as / transitive-1-level / function type (§17.4) — the reinterpretation
// precursor. REJECTED.
BEZEL_TEST(is_transitive_generic_2192, as_transitive_function_typearg_collapsed_reject_2192_17_4) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(o: Object): int "
        "  var w2: Wrap<T>? := o as Wrap<T>; "
        "  return (if w2 <> null then 7 else 9); "
        "  end probe; end A; "
        "class B<U> public method run(o: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(o); end run; end B; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(1); "
        "  var o: Object := w; "
        "  using var b := new B<(int) -> int>(); "
        "  return b.run(o); end Main; end C;"));
}

// (10) case (statement) / transitive-1-level / function type (§19.3). REJECTED.
BEZEL_TEST(is_transitive_generic_2192, case_stmt_transitive_function_typearg_collapsed_reject_2192_19_3) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(o: Object): int "
        "  var r := 9; "
        "  case o of var x: Wrap<T> => do r := 7; end, _ => do r := 9; end end; "
        "  return r; end probe; end A; "
        "class B<U> public method run(o: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(o); end run; end B; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(1); "
        "  var o: Object := w; "
        "  using var b := new B<(int) -> int>(); "
        "  return b.run(o); end Main; end C;"));
}

// (11) is / transitive-1-level / NESTED function type (§19.3, #2184). The outer
// Named reifies but the inner argument is a function type — the descriptor
// collapses at DEPTH; the STRICT recursive oracle catches it. REJECTED.
BEZEL_TEST(is_transitive_generic_2192, is_nested_transitive_function_typearg_collapsed_reject_2192_19_3) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Inner<V> end Inner; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(o: Object): int "
        "  => (if o is Wrap<Inner<T>> then 7 else 9); end A; "
        "class B<U> public method run(o: Object): int "
        "  using var a := new A<U>(); "
        "  return a.probe(o); end run; end B; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<Inner<(string) -> string>>(1); "
        "  var o: Object := w; "
        "  using var b := new B<(int) -> int>(); "
        "  return b.run(o); end Main; end C;"));
}

// (12) catch / transitive-1-level / function type (§31.2.1). Outer<(int)->int>
// -> Thrower<(int)->int>; the `catch var e1: MyExc<T=(int)->int>` arm against a
// thrown MyExc<(string)->string>. WITHOUT the fix the collapsed arm caught it
// (over-match, wrong static type -> 1). Now REJECTED.
BEZEL_TEST(is_transitive_generic_2192, catch_transitive_function_typearg_collapsed_reject_2192_31_2_1) {
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
        "  return 0; end go; end Thrower; "
        "class Outer<U> public method run(): int "
        "  using var t := new Thrower<U>(); "
        "  return t.go(); end run; end Outer; "
        "class C public static method Main(): int "
        "  using var o := new Outer<(int) -> int>(); "
        "  return o.run(); end Main; end C;"));
}

// ==================== NON-REGRESSION: the DIRECT path is untouched ===========
// (empty outer substitution -> substituteTypeRefLocal is identity, so behavior
// is exactly as #2012 / #1843 produced it)

// (13) is / DIRECT / function type (§14.2, #2012). `new A<(int)->int>()` written
// directly in Main (no enclosing generic): the collapsed target is still
// REJECTED — the fix must not defeat the #2012 direct guard.
BEZEL_TEST(is_transitive_generic_2192, is_direct_function_typearg_still_rejects_2192_14_2) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(o: Object): int "
        "  => (if o is Wrap<T> then 7 else 9); end A; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(1); "
        "  var o: Object := w; "
        "  using var a := new A<(int) -> int>(); "
        "  return a.probe(o); end Main; end C;"));
}

// (14) is / DIRECT / reified (§19.3, #1843). `new A<int>()` written directly:
// `o is Wrap<T=int>` still runs the PRECISE per-instantiation answer (match a
// Wrap<int>, not a Wrap<string>) — the fix must not perturb the direct reified
// path. Encoded 2.
BEZEL_TEST(is_transitive_generic_2192, is_direct_reified_still_matches_2192_19_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class A<T> public method probe(m: Object, w: Object): int "
        "  => (if m is Wrap<T> then 1 else 0) * 2 "
        "   + (if w is Wrap<T> then 1 else 0); end A; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<int>(1); "
        "  using var ws := new Wrap<string>(2); "
        "  var mi: Object := wi; var wsx: Object := ws; "
        "  using var a := new A<int>(); "
        "  return a.probe(mi, wsx); end Main; end C;"), 2);
}
