// fix #3578 / stdlib §4.17 (v0.285): SortedDictionary<K, V>.Contains(key: K)
// is declared to return `bool` (the §4.8 `key in dict` alias). The compiler
// was instead typing the call as the VALUE type V — silent for V = int, but
// at V = string it emitted malformed IR (an `i1` consumed as `%bz.string*`,
// the IR verifier aborting with "GEP base pointer is not a vector..." /
// "Load operand must be a pointer") and the compile aborted. `ContainsKey`
// was already correctly typed — the bug was strictly in the `Contains` alias
// being absent from the SortedDictionary bool-returning member list at
// src/sema/ExprTypecheck_Calls.cpp.
//
// Pre-fix repro (per issue):
//
//   module M;
//   class C
//     public static method Main(): int
//       var sd := new SortedDictionary<int, string>();
//       sd.Add(2, "b"); sd.Add(1, "a");
//       var ct := sd.Contains(2);
//       Console.WriteLine($"ct={ct}");
//       return 0;
//     end Main;
//   end C;
//
// → IR verifier aborts; compile never finishes. Post-fix: prints `ct=True`
// and exits 0.
//
// Spec refs: stdlib §4.17 (SortedDictionary.Contains / ContainsKey, v0.285).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// AOT-compile+run a program and return its `Main` exit code (or -999 if it
// did not run: compile/link reject or program crash/timeout).
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Plain program wrapper — no resource class.
std::string wrap(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §4.17 (v0.285) — SortedDictionary<K, V>.Contains(key: K): bool
// ---------------------------------------------------------------------------

// stdlib §4.17 / §4.8 `key in dict` alias: V = int. The result is bool, so
// `Contains` is usable directly as an `if` condition and is bound to bool.
// Pre-fix this was a silent V-mistyping (the call was typed as int).
// Encoded: a*8 + (not b)*4 + (Count = 2)*2 + (sd[1] = 100) -> 15.
BEZEL_TEST(sorted_dictionary_contains_3578, sorted_dictionary_int_value_contains_is_bool_4_17) {
    BEZEL_EXPECT_EQ(run(wrap(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.Add(2, 200); sd.Add(1, 100); "
        "var a := sd.Contains(1); "
        "var b := sd.Contains(99); "
        "return int(a) * 8 + int(not b) * 4 + "
        "       int(sd.Count = 2) * 2 + int(sd[1] = 100); ")), 15);
}

// §5.1 + stdlib §4.17: the result is `bool`, so binding it to an `int` target
// is a §5.1 reject — codegen alone does not fix this; without the typing arm
// the call is left untyped and this program COMPILES, a silent §5.1 hole.
BEZEL_TEST(sorted_dictionary_contains_3578, sorted_dictionary_contains_bool_not_int_5_1) {
    auto r = runAot(wrap(
        "var sd := new SortedDictionary<int, int>(); "
        "var b: int := sd.Contains(1); "
        "return b; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("not implicitly convertible") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// stdlib §4.17: the §4.8 `key in dict` syntax (`in` operator) lowers to the
// same `Contains` member on a SortedDictionary. The `in` path goes through a
// DIFFERENT ExprTypecheck arm (not the CallExpr block where the bug lived),
// so this is included as a guard against accidentally regressing the `in`
// path while patching the dot-call path.
BEZEL_TEST(sorted_dictionary_contains_3578, sorted_dictionary_in_operator_int_value_4_17) {
    BEZEL_EXPECT_EQ(run(wrap(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.Add(2, 200); sd.Add(1, 100); "
        "var a := 1 in sd; "
        "var b := 99 in sd; "
        "return int(a) * 8 + int(not b) * 4 + "
        "       int(sd.Count = 2) * 2 + int(sd[1] = 100); ")), 15);
}

// ---------------------------------------------------------------------------
// stdlib §4.17 (v0.285) — SortedDictionary<int, string>.Contains: the crash
// from the issue body. Pre-fix: IR verifier abort; post-fix: compiles + runs
// and prints `ct=True`.
// ---------------------------------------------------------------------------

// stdlib §4.17: the canonical repro from the issue. V = string — pre-fix this
// fails to compile (IR verifier rejects the malformed i1-as-string); post-fix
// it prints "ct=True" and returns 0.
BEZEL_TEST(sorted_dictionary_contains_3578, sorted_dictionary_string_value_contains_issue_3578) {
    BEZEL_EXPECT_EQ(runAotOut(wrap(
        "var sd := new SortedDictionary<int, string>(); "
        "sd.Add(2, \"b\"); sd.Add(1, \"a\"); "
        "var ct := sd.Contains(2); "
        "Console.WriteLine($\"ct={ct}\"); "
        "return 0; ")), "ct=True");
}

// stdlib §4.17: present/absent contract on a string-valued map. Pre-fix:
// IR verifier abort (malformed IR); post-fix: 7 (= 1*8 + 1*4 + 0*2 + 1).
// Encoded: a*8 + b*4 + (not c)*2 + (sd[1] = "a") -> 15.
BEZEL_TEST(sorted_dictionary_contains_3578, sorted_dictionary_string_value_present_absent_4_17) {
    BEZEL_EXPECT_EQ(run(wrap(
        "var sd := new SortedDictionary<int, string>(); "
        "sd.Add(2, \"b\"); sd.Add(1, \"a\"); "
        "var a := sd.Contains(1); "
        "var b := sd.Contains(2); "
        "var c := sd.Contains(99); "
        "return int(a) * 8 + int(b) * 4 + int(not c) * 2 + "
        "       int(sd[1] = \"a\"); ")), 15);
}

// stdlib §4.17: result usable directly as a condition on a string-valued
// map. Pre-fix: IR verifier abort; post-fix: 3.
BEZEL_TEST(sorted_dictionary_contains_3578, sorted_dictionary_string_value_contains_as_condition_4_17) {
    BEZEL_EXPECT_EQ(run(wrap(
        "var sd := new SortedDictionary<int, string>(); "
        "sd.Add(2, \"b\"); sd.Add(1, \"a\"); "
        "var n := 0; "
        "if sd.Contains(1) then n := n + 1; end; "
        "if not sd.Contains(99) then n := n + 2; end; "
        "return n; ")), 3);
}

// stdlib §4.17: the string-keyed form. §28.9 +1 retention across the
// Contains read is a no-op (Contains does not touch values) — the entries
// from `Add` are still disposed correctly at scope end. LSan is the oracle.
// Encoded: 1 (a) + 1 (b) + 0 (not c) -> 3 (top 3 bits), plus 8 (Count = 2)
// + 4 (sd["b"] = "y") -> 15.
BEZEL_TEST(sorted_dictionary_contains_3578, sorted_dictionary_string_key_string_value_contains_4_17) {
    BEZEL_EXPECT_EQ(run(wrap(
        "var sd := new SortedDictionary<string, string>(); "
        "sd.Add(\"b\", \"y\"); sd.Add(\"a\", \"x\"); "
        "var a := sd.Contains(\"a\"); "
        "var b := sd.Contains(\"b\"); "
        "var c := sd.Contains(\"z\"); "
        "return int(a) * 8 + int(b) * 4 + int(not c) * 2 + "
        "       int(sd.Count = 2); ")), 15);
}
