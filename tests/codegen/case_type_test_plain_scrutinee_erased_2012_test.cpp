// #2012 (§12.9/§14.2/§17.4, v0.197) — the runtime half of the case-binding
// erasure guard for a PLAIN scrutinee (src/sema/ExprTypecheck.cpp, the CaseStmt
// handler). The sema half (tests/sema/case_type_test_plain_scrutinee_erased_2012)
// pins that a FUNCTION-typed generic target is REJECTED at compile time on this
// path (parity with #2006's plain `is`/`as` guard). This half pins BOTH sides at
// runtime:
//   * the memory-unsafe FALSE POSITIVE the fix eliminates — a `Wrap<(int)->int>`
//     value matching a `case o of var x: Wrap<(string)->string>` arm — no longer
//     compiles (FAILS WITHOUT the fix, where it compiled and returned a bad
//     match; PASSES WITH: the program is rejected, so runAot reports ran=false);
//   * the REIFIED targets that stay admitted still compile AND return the CORRECT,
//     PRECISE (#2001/#1843) runtime answer — the guard must not over-reject.
//
// These use the STATEMENT form `case ... => do ... end` (a CaseStmt) so they drive
// the ExprTypecheck.cpp handler under test.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A phantom `Wrap<W>` (W is only a type argument, never stored — isolates the
// descriptor key) plus a Main body. Returns the exit code, or -999 if the program
// did not compile+run (used to observe the compile-time rejection).
int runProg(const std::string& mainBody) {
    auto r = runAot(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Foo end Foo; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// (1) HEADLINE: the memory-unsafe false positive is gone. A `Wrap<(int)->int>`
// value under an `Object` scrutinee, tested against `var x: Wrap<(string)->string>`.
// WITHOUT the fix this compiled and the arm MATCHED (a value of one function-type
// instantiation bound at another → type confusion), returning 1. WITH the fix the
// program is REJECTED at compile time, so it never runs → -999.
BEZEL_TEST(codegen_2012, function_arg_false_positive_no_longer_compiles) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<(int) -> int>(7); "
        "var o: Object := w; "
        "var r := 0; "
        "case o of "
        "  var x: Wrap<(string) -> string> => do r := 1; end, "
        "  _ => do r := 0; end "
        "end; "
        "return r;"), -999);
}

// (2) Concrete PRIMITIVE argument stays admitted + PRECISE: a `Wrap<int>` value
// matches `var x: Wrap<int>` (1) and does NOT match `var y: Wrap<string>` (0).
BEZEL_TEST(codegen_2012, primitive_arg_is_precise) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<int>(7); "
        "var o: Object := w; "
        "var a := 0; "
        "case o of var x: Wrap<int> => do a := 1; end, _ => do a := 0; end end; "
        "var b := 0; "
        "case o of var y: Wrap<string> => do b := 1; end, _ => do b := 0; end end; "
        "return a * 2 + b;"), 2);
}

// (3) Concrete NAMED-class argument stays admitted + precise.
BEZEL_TEST(codegen_2012, named_class_arg_is_precise) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<Foo>(7); "
        "var o: Object := w; "
        "var a := 0; "
        "case o of var x: Wrap<Foo> => do a := 1; end, _ => do a := 0; end end; "
        "var b := 0; "
        "case o of var y: Wrap<int> => do b := 1; end, _ => do b := 0; end end; "
        "return a * 2 + b;"), 2);
}

// (4) FOLDED inline-array argument (post-#2001): `Wrap<[3]int>` is admitted and
// distinguished from `Wrap<[4]int>` by size — the guard must not undo #2001.
BEZEL_TEST(codegen_2012, inline_array_arg_is_precise) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<[3]int>(7); "
        "var o: Object := w; "
        "var a := 0; "
        "case o of var x: Wrap<[3]int> => do a := 1; end, _ => do a := 0; end end; "
        "var b := 0; "
        "case o of var y: Wrap<[4]int> => do b := 1; end, _ => do b := 0; end end; "
        "return a * 2 + b;"), 2);
}

// (5) BARE TYPE PARAMETER in a generic body (the #1843 shape) stays admitted and
// resolves to the concrete argument at monomorphization: `var x: Wrap<T>` with T
// bound to int matches a `Wrap<int>` value. The over-rejection guard, at runtime.
BEZEL_TEST(codegen_2012, bare_type_parameter_resolves_at_monomorphization) {
    auto r = runAot(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Checker<T> "
        "  public method probe(m: Object): int "
        "    var r := 0; "
        "    case m of var x: Wrap<T> => do r := 1; end, _ => do r := 0; end end; "
        "    return r; "
        "  end probe; "
        "end Checker; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<int>(1); "
        "  using var ch := new Checker<int>(); "
        "  var mi: Object := wi; "
        "  return ch.probe(mi); "
        " end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}
