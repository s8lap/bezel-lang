// fix #1758 / §27.6: a bare reference to a PRIMITIVE-INTRINSIC static method —
// `int.Parse`, `char.IsDigit`, `double.Parse`, … used as a function VALUE (not
// immediately called) — must form the method-reference function value §27.6
// prescribes and lower to a callable closure. `strings.Select(int.Parse)` is
// the spec's OWN §27.6 worked example ("When a lambda's body is just a call to
// an existing method, the method reference may be used directly").
//
// BUG (#1758, split from #1692): a primitive intrinsic is recognized by NAME in
// sema/codegen — it has no `MethodSymbol` backing an `ast::MethodDecl`. So the
// two mechanisms that make an ordinary `obj.Method` reference work both misfired
// on it:
//   * sema typed the bare `int.Parse` access as its scalar RESULT type (`int`),
//     never as the function type `(string) -> int`. Harmless when the reference
//     is immediately called (`int.Parse(s)` — typeCall dispatches on the member,
//     not the access's own type), but as a bare value it left the reference
//     scalar-typed: `strings.Select(int.Parse)` then inferred its element type
//     as the SOURCE element (`string`), surfacing as a spurious `sum + x` type
//     error on the very example §27.6 gives — and an explicit
//     `IEnumerable<int>` annotation instead compiled to a SIGSEGV.
//   * codegen's method-reference-to-closure path keys off a `MethodSymbol`;
//     with none, the bare `int.Parse` value lowered to a null function pointer,
//     which SIGSEGVs when the lazy pipeline invokes it per element.
//
// Two-layer fix:
//   * sema (resolveMemberAccess primitive-keyword block): when a recognized
//     primitive-intrinsic static METHOD is NOT the direct callee of a call,
//     type it as `(paramTypes) -> returnType` (a method reference, §27.6)
//     instead of its scalar result. The direct-callee case is unchanged (the
//     scalar keeps the #1093 delegate arity-check from misfiring).
//   * codegen (makePrimIntrinsicThunk): back the reference with a thunk closure
//     `(ptr env, params…) -> ret` that re-lowers the intrinsic through the same
//     tryLowerPrimitiveStaticCall path a direct call uses — so every primitive-
//     static member reachable as a call is reachable as a reference, with no
//     per-member codegen.
//
// These tests AOT-run programs that take a bare primitive-intrinsic method
// reference and invoke it (directly and through LINQ). Without the fix each
// either fails to compile (the scalar reference is not assignable to the
// function type — sentinel -999) or SIGSEGVs at runtime (null fn pointer).
//
// NOTE: the issue's verbatim repro annotates the local
// `var parsed: IEnumerable<int> := strings.Select(int.Parse)`. That form trips a
// SEPARATE, pre-existing bug (a lazy Select/Where result widened to an explicit
// `IEnumerable<T>` local SIGSEGVs on enumeration — reproduces with a plain
// lambda selector too, so it is independent of method references; filed
// separately). These tests therefore pin the method-reference fix through the
// inferred-local / aggregate forms, which exercise the same thunk without the
// unrelated annotation path.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Run a whole module source and return Main's exit code, or a sentinel.
int runMod(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §27.6 (#1758): `int.Parse` bound to a `(string) -> int` local and called —
// the minimal bare-method-reference repro. Without the fix: compile error
// (-999), the scalar `int` reference is not assignable to `(string) -> int`.
// Expected 42 (Parse("42")).
BEZEL_TEST(codegen_primitive_intrinsic_method_ref_1758, int_parse_typed_local_ref) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var f: (string) -> int := int.Parse; "
        "    return f(\"42\"); "
        "  end Main; "
        "end C;"), 42);
}

// §27.6 (#1758): the spec's OWN worked example — `strings.Select(int.Parse)` —
// materialized with `.Sum()`. Without the fix the selector's element type was
// inferred as the source element (`string`) rather than `int.Parse`'s return
// type, so `.Sum()` (or an int consumer) miscompiled / rejected the program.
// Expected 42 (10 + 20 + 12).
BEZEL_TEST(codegen_primitive_intrinsic_method_ref_1758, select_int_parse_sum_spec_example) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Main(): int "
        "    using var strings := new Array<string>(); "
        "    strings.Add(\"10\"); strings.Add(\"20\"); strings.Add(\"12\"); "
        "    return strings.Select(int.Parse).Sum(); "
        "  end Main; "
        "end C;"), 42);
}

// §27.6 (#1758): the spec example enumerated element by element — the driving
// case from the issue (inferred local, avoiding the unrelated IEnumerable-
// annotation bug). Without the fix `for var x in parsed` bound `x` as `string`
// and `sum + x` was an int+string type error. Expected 6 (1 + 2 + 3).
BEZEL_TEST(codegen_primitive_intrinsic_method_ref_1758, select_int_parse_for_enumeration) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Main(): int "
        "    using var strings := new Array<string>(); "
        "    strings.Add(\"1\"); strings.Add(\"2\"); strings.Add(\"3\"); "
        "    var parsed := strings.Select(int.Parse); "
        "    var sum := 0; "
        "    for var x in parsed do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C;"), 6);
}

// §27.6 (#1758): a char classification intrinsic — `char.IsDigit` as a
// `(char) -> bool` value. Guards the single-arg predicate signature and the
// thunk's char (i32) parameter. Expected 42 (IsDigit('7') && !IsDigit('x')).
BEZEL_TEST(codegen_primitive_intrinsic_method_ref_1758, char_isdigit_typed_local_ref) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var p: (char) -> bool := char.IsDigit; "
        "    if p('7') and not p('x') then return 42; else return 0; end; "
        "  end Main; "
        "end C;"), 42);
}

// §27.6 (#1758): a primitive-intrinsic predicate driving a LINQ `Where` —
// `cs.Where(char.IsDigit)` counts the digit chars. Exercises the predicate
// trampoline (kind 0) invoking the thunk. Expected 3 ('3','7','9').
BEZEL_TEST(codegen_primitive_intrinsic_method_ref_1758, where_char_isdigit_predicate) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Main(): int "
        "    using var cs := new Array<char>(); "
        "    cs.Add('a'); cs.Add('3'); cs.Add('7'); cs.Add('x'); cs.Add('9'); "
        "    var n := 0; "
        "    for var c in cs.Where(char.IsDigit) do n := n + 1; end; "
        "    return n; "
        "  end Main; "
        "end C;"), 3);
}

// §27.6 (#1758): a non-int-returning intrinsic — `double.Parse` as a
// `(string) -> double` selector — verifies the thunk's f64 return flows through
// the selector trampoline correctly. Returns 1 iff 1.5 + 2.5 == 4.0.
BEZEL_TEST(codegen_primitive_intrinsic_method_ref_1758, select_double_parse_value) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Main(): int "
        "    using var strings := new Array<string>(); "
        "    strings.Add(\"1.5\"); strings.Add(\"2.5\"); "
        "    var total := 0.0; "
        "    for var v in strings.Select(double.Parse) do total := total + v; end; "
        "    if total = 4.0 then return 1; else return 2; end; "
        "  end Main; "
        "end C;"), 1);
}

// §27.6 (#1758): one reference value invoked repeatedly with different
// arguments — the thunk closes over no receiver, so each call is independent.
// Expected 42 (Parse("40") + Parse("2")).
BEZEL_TEST(codegen_primitive_intrinsic_method_ref_1758, int_parse_ref_multiple_calls) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var f: (string) -> int := int.Parse; "
        "    var sum := f(\"40\"); "
        "    sum := sum + f(\"2\"); "
        "    return sum; "
        "  end Main; "
        "end C;"), 42);
}

// Regression (§3.4 / #1093): the DIRECT call form `int.Parse(s)` is unchanged —
// as the callee of a call the access keeps its scalar result type, so the call
// dispatches exactly as before (guards the callee-position flag that gates the
// method-reference typing above). Expected 42 (40 + 2).
BEZEL_TEST(codegen_primitive_intrinsic_method_ref_1758, direct_int_parse_call_regression) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var a := int.Parse(\"40\"); "
        "    var b := int.Parse(\"2\"); "
        "    return a + b; "
        "  end Main; "
        "end C;"), 42);
}
