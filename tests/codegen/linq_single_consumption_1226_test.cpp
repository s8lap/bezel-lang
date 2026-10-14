// #1226 / §29.11 v0.156: a bound lazy LINQ query is single-consumption
// (affine).  The first for-loop (or terminal op) CONSUMES the lazy pipeline;
// a second enumeration must be:
//   (a) a COMPILE ERROR where statically detectable (DA pass, §29.11), or
//   (b) a runtime InvalidOperationException (the slot is nulled after the
//       first loop, detected by the null-guard in lowerLazyLinqFor).
//
// Previously (pre-#1226, post-#1218 fix) the second enumeration yielded
// silently-empty — wrong value, no signal to the programmer.
//
// These tests verify:
//   1. STATIC: second for-loop over a consumed bound lazy is a compile error;
//      RUNTIME: in a callee's plain IEnumerable<T> parameter (statically
//      untracked) it raises InvalidOperationException (§29.11 v0.338)
//   2. RUNTIME: first enumeration still yields the correct result (s1 = 6)
//   3. RUNTIME: fresh inline lazy (`for x in a.Where(…)`) still works once
//   4. RUNTIME: `var ys := a.Where(…).ToArray()` materialises → re-enumerable
//   5. STATIC: DA detects sequential double-enumeration in full-compiler path
//   6. STATIC: DA accepts single-enumeration (no false positive)
//   7. STATIC: DA accepts if-branch conditional (no false positive)
//   8. RUNTIME: multiple independent bound lazies, each enumerated once → OK
//
// Spec citations: §29.11, §5 stdlib lazy IEnumerable semantics.

#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Run via AOT; return the int return value, or -999 on compile/link/run failure.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// True iff the program is REJECTED at compile time and some diagnostic message
// contains every needle (a specific diagnostic, not merely "failed").
bool rejectedWith(const std::string& src, std::initializer_list<const char*> needles) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        bool all = true;
        for (const char* n : needles)
            if (d.message.find(n) == std::string::npos) all = false;
        if (all) return true;
    }
    return false;
}

// Run the FULL sema pipeline (including checkDefiniteAssignment) and return
// true iff at least one error diagnostic was reported.
// This function drives the sema pipeline that the `bezelc` driver runs, so
// DA-based static errors are visible.
bool rejectedByDA(const std::string& src) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return diags.hasErrors();
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    if (diags.hasErrors()) return true; // earlier sema errors
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    return diags.hasErrors();
}

// True iff at least one diagnostic message contains `needle`.
bool daErrorContains(const std::string& src, const std::string& needle) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return false;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    for (const auto& d : diags.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// ── RUNTIME: second for-loop raises InvalidOperationException ──────────────

// §29.11 v0.338: a lazy query "is single-consumption. It may be enumerated at
// most once ... an affine discipline"; a second enumeration "is a compile error
// where the flow analysis can see it statically".  The bound `filtered` is
// enumerated twice in straight-line code, so the AOT driver REJECTS it.
// FLIP (#1226): this row used to assert a RUNTIME InvalidOperationException
// (rc 601), reachable only because the JIT skipped the ownership pass.
BEZEL_TEST(linq_single_consumption_1226,
           second_forin_rejected_at_compile_time_spec_29_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var filtered := a.Where(lambda(x) => x mod 2 = 0); "
        "  var s1 := 0; for var x in filtered do s1 := s1 + x; end; "
        "  var caught := 0; "
        "  try "
        "    for var x in filtered do caught := caught + x; end; "
        "  catch InvalidOperationException => "
        "    caught := 1; "
        "  end; "
        "  return s1 * 100 + caught; "
        "end Main; end C;",
        {"bound lazy query `filtered` was already consumed", "§29.11"}));
}

// §29.11 v0.338: "Inside a callee, a plain IEnumerable<T> parameter is
// statically untracked ... so a violation there is enforced only by the runtime
// InvalidOperationException guard."  The callee's second loop over `xs` raises
// IOE: s1=6, caught=1 -> 601 (printed: outside 0..255).
BEZEL_TEST(linq_single_consumption_1226,
           callee_param_second_forin_raises_ioe_spec_29_11) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "public static method Twice(xs: IEnumerable<int>): int "
        "  var s1 := 0; for var x in xs do s1 := s1 + x; end; "
        "  var caught := 0; "
        "  try "
        "    for var x in xs do caught := caught + x; end; "
        "  catch InvalidOperationException => "
        "    caught := 1; "
        "  end; "
        "  return s1 * 100 + caught; "
        "end Twice; "
        "public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var filtered := a.Where(lambda(x) => x mod 2 = 0); "
        "  Console.WriteLine(C.Twice(filtered)); "
        "  return 0; "
        "end Main; end C;"), "601");
}

// §29.11: the first enumeration must still yield the correct value (s=6).
BEZEL_TEST(linq_single_consumption_1226,
           first_enumeration_correct_value_spec_29_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var filtered := a.Where(lambda(x) => x mod 2 = 0); "
        "  var s := 0; for var x in filtered do s := s + x; end; "
        "  return s; "
        "end Main; end C;"), 6);
}

// ── RUNTIME: fresh inline lazy still works ─────────────────────────────────

// §29.11: a fresh inline lazy (not bound to a var) is never null — it can
// be enumerated normally.  The null-guard does not fire.
BEZEL_TEST(linq_single_consumption_1226,
           fresh_inline_lazy_unaffected_spec_29_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var s := 0; "
        "  for var x in a.Where(lambda(x) => x mod 2 = 0) do s := s + x; end; "
        "  return s; "
        "end Main; end C;"), 6);
}

// ── RUNTIME: .ToArray() materialises → re-enumerable ──────────────────────

// §29.11: `var ys := a.Where(…).ToArray()` produces an owned Array<int> that
// is re-enumerable any number of times.  Both for-loops must yield s=6.
// 606 is outside 0..255, so it is asserted on stdout (runAotOut).
BEZEL_TEST(linq_single_consumption_1226,
           toarray_materializes_reenumerable_spec_29_11) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  using var ys: Array<int> := a.Where(lambda(x) => x mod 2 = 0).ToArray(); "
        "  var s1 := 0; for var x in ys do s1 := s1 + x; end; "
        "  var s2 := 0; for var x in ys do s2 := s2 + x; end; "
        "  Console.WriteLine(s1 * 100 + s2); "
        "  return 0; "
        "end Main; end C;"), "606");
}

// ── RUNTIME: two independent bound lazies, each enumerated once → OK ───────

// §29.11: two distinct bound lazies may each be enumerated once without error.
// f1 yields {2,4}→s1=6; f2 yields {1,3}→s2=4; rc=s1*10+s2=64.
BEZEL_TEST(linq_single_consumption_1226,
           two_independent_lazies_each_enumerated_once_spec_29_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var f1 := a.Where(lambda(x) => x mod 2 = 0); "
        "  var f2 := a.Where(lambda(x) => x mod 2 = 1); "
        "  var s1 := 0; for var x in f1 do s1 := s1 + x; end; "
        "  var s2 := 0; for var x in f2 do s2 := s2 + x; end; "
        "  return s1 * 10 + s2; "
        "end Main; end C;"), 64);
}

// ── STATIC: DA detects sequential double-enumeration ──────────────────────

// §29.11 / #1226: the DA pass (called by the full compiler, not JIT) must
// emit a compile error when `filtered` is enumerated a second time in
// straight-line code.
BEZEL_TEST(linq_single_consumption_1226,
           da_rejects_sequential_double_enumeration_spec_29_11) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var filtered := a.Where(lambda(x) => x mod 2 = 0); "
        "  var s1 := 0; for var x in filtered do s1 := s1 + x; end; "
        "  var s2 := 0; for var x in filtered do s2 := s2 + x; end; "
        "  return s1 * 100 + s2; "
        "end Main; end C;"), true);
}

// §29.11 / #1226: the error message should mention the variable name and §29.11.
BEZEL_TEST(linq_single_consumption_1226,
           da_error_cites_variable_name_spec_29_11) {
    BEZEL_EXPECT_EQ(daErrorContains(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  var filtered := a.Where(lambda(x) => x > 0); "
        "  var s1 := 0; for var x in filtered do s1 := s1 + x; end; "
        "  var s2 := 0; for var x in filtered do s2 := s2 + x; end; "
        "  return s1 + s2; "
        "end Main; end C;", "filtered"), true);
}

// ── STATIC: DA accepts single enumeration (no false positive) ─────────────

// §29.11: a single enumeration of a bound lazy must NOT be rejected.
BEZEL_TEST(linq_single_consumption_1226,
           da_accepts_single_enumeration_spec_29_11) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var filtered := a.Where(lambda(x) => x mod 2 = 0); "
        "  var s := 0; for var x in filtered do s := s + x; end; "
        "  return s; "
        "end Main; end C;"), false);
}

// ── STATIC: DA is conservative for conditional enumerations ───────────────

// §29.11: a lazy var enumerated only in one branch of an if (without else)
// is NOT definitely consumed — the second for-loop is reachable and must NOT
// be flagged as a compile error (the runtime guard handles dynamic cases).
BEZEL_TEST(linq_single_consumption_1226,
           da_conservative_for_conditional_enumeration_spec_29_11) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var filtered := a.Where(lambda(x) => x mod 2 = 0); "
        "  var s1 := 0; "
        "  if a.Count > 0 then "
        "    for var x in filtered do s1 := s1 + x; end; "
        "  end; "
        "  var s2 := 0; "
        "  for var x in filtered do s2 := s2 + x; end; "
        "  return s1 + s2; "
        "end Main; end C;"), false);
}

// ── STATIC: DA detects double consumption in both branches of if-else ──────

// §29.11: if `filtered` is consumed in BOTH branches of an if-else, it is
// definitely consumed after the if — a subsequent for-loop is a compile error.
BEZEL_TEST(linq_single_consumption_1226,
           da_rejects_both_branches_then_use_spec_29_11) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var filtered := a.Where(lambda(x) => x mod 2 = 0); "
        "  var s := 0; "
        "  if a.Count > 2 then "
        "    for var x in filtered do s := s + x; end; "
        "  else "
        "    for var x in filtered do s := s + x; end; "
        "  end; "
        "  for var x in filtered do s := s + x; end; "
        "  return s; "
        "end Main; end C;"), true);
}

// ── #1252: a TERMINAL op consumes a bound lazy too (UAF → IOE) ─────────────

// §29.11 v0.338 consumption point 1 ("Enumeration or a terminal operator"): a
// terminal op (Count) spends the bound query's enumeration right; a subsequent
// for-loop is a second enumeration, a compile error where visible (#6197).
// FLIP (#6197): was a runtime IOE (rc 201); callee twin below keeps the runtime.
BEZEL_TEST(linq_single_consumption_1226,
           terminal_then_forin_raises_ioe_1252_spec_29_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var n := q.Count(); "
        "  var caught := 0; "
        "  for var x in q do caught := caught + x; end; "
        "  return n * 100 + caught; "

        "end Main; end C;",
        {"bound lazy query `q` was already consumed", "§29.11"}));
}

// §29.11 v0.338 consumption point 1: second terminal op on a spent query.
// FLIP (#6197): was a runtime IOE (rc 201).
BEZEL_TEST(linq_single_consumption_1226,
           second_terminal_raises_ioe_1252_spec_29_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var n := q.Count(); "
        "  var r := q.Sum(); "
        "  return n * 100 + r; "

        "end Main; end C;",
        {"bound lazy query `q` was already consumed", "§29.11", "terminal operator"}));
}

// §29.11 v0.338: inside a callee a plain IEnumerable<T> parameter is statically
// untracked; the runtime IOE guard still fires (n=2, caught=1 -> 201).
BEZEL_TEST(linq_single_consumption_1226,
           callee_param_terminal_then_forin_raises_ioe_spec_29_11) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "public static method Run(xs: IEnumerable<int>): int "
        "  var n := xs.Count(); "
        "  var caught := 0; "
        "  try "
        "    for var x in xs do caught := caught + x; end; "
        "  catch InvalidOperationException => "
        "    caught := 1; "
        "  end; "
        "  return n * 100 + caught; "
        "end Run; "
        "public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  Console.WriteLine(C.Run(q)); "
        "  return 0; "
        "end Main; end C;"), "201");
}

// §29.11 / #1252: a single terminal op on a bound lazy is correct (no spurious
// throw) — Count over {2,3} of a.Where(x>1) = 2.
BEZEL_TEST(linq_single_consumption_1226,
           single_terminal_bound_lazy_correct_1252_spec_29_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  return q.Count(); "
        "end Main; end C;"), 2);
}

// §29.11 / #1252: a fresh inline lazy terminal (no bound slot) is unaffected by
// the null-guard — Count = 2.
BEZEL_TEST(linq_single_consumption_1226,
           fresh_inline_terminal_unaffected_1252_spec_29_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return a.Where(lambda(x) => x > 1).Count(); "
        "end Main; end C;"), 2);
}

// ── §29.11 point 2 (#1353): applying a further deferred op consumes `q` ───

// §29.11 v0.165 / #1353: `var q2 := q.Select(f)` consumes the binding `q`.
// A subsequent for-loop over `q` must be a static compile error.
BEZEL_TEST(linq_single_consumption_1226,
           da_rejects_chain_then_enumeration_spec_29_11_pt2) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 0); "
        "  var q2 := q.Select(lambda(x) => x * 2); "  // consumes q
        "  var s := 0; "
        "  for var x in q do s := s + x; end; "  // DA error: q consumed
        "  return s; "
        "end Main; end C;"), true);
}

// §29.11 v0.165 / #1353: chaining once into q2 then using q2 once is fine
// (no false positive on q2).
BEZEL_TEST(linq_single_consumption_1226,
           da_accepts_chain_single_use_spec_29_11_pt2) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 0); "
        "  var q2 := q.Select(lambda(x) => x * 2); "  // q consumed, q2 fresh
        "  var s := 0; "
        "  for var x in q2 do s := s + x; end; "  // OK: q2 consumed once
        "  return s; "
        "end Main; end C;"), false);
}

// §29.11 v0.165 / #1353: chaining then applying a second chain on q (already
// consumed by first chain) is a static error.
BEZEL_TEST(linq_single_consumption_1226,
           da_rejects_double_chain_spec_29_11_pt2) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 0); "
        "  var q2 := q.Select(lambda(x) => x * 2); "  // q consumed
        "  var q3 := q.Skip(1); "               // DA error: q already consumed
        "  var s := 0; "
        "  for var x in q3 do s := s + x; end; "
        "  return s; "
        "end Main; end C;"), true);
}

// ── §29.11 point 3 (#1353): passing query as IEnumerable<T> arg consumes it ─

// §29.11 v0.165 / #1353: passing `q` to a method that takes IEnumerable<int>
// consumes the binding; a subsequent for-loop over `q` is a static error.
// Use ToArray() (which takes IEnumerable<T>) as a proxy method that accepts
// a lazy query — passing q to ToArray() is a valid IEnumerable<T> argument.
// Note: a chained q.ToArray() (Point 1 terminal) is the same effect; here
// we test passing q as an argument to a NEW constructor or a LINQ operator
// that takes IEnumerable<T> explicitly (e.g. a.Concat(q)).
BEZEL_TEST(linq_single_consumption_1226,
           da_rejects_ienumerable_arg_then_enumeration_spec_29_11_pt3) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 0); "
        "  var b := new Array<int>(); "
        "  var merged := b.Concat(q); "  // q passed as IEnumerable<int> arg → consumed
        "  var s := 0; "
        "  for var x in q do s := s + x; end; "  // DA error: q consumed
        "  return s; "
        "end Main; end C;"), true);
}

// §29.11 v0.165 / #1353: passing `q` as argument once is fine (no false positive).
BEZEL_TEST(linq_single_consumption_1226,
           da_accepts_single_ienumerable_arg_spec_29_11_pt3) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 0); "
        "  var b := new Array<int>(); "
        "  var merged := b.Concat(q); "  // q consumed (first use, OK)
        "  var s := 0; "
        "  for var x in merged do s := s + x; end; "  // merged is a fresh lazy, OK
        "  return s; "
        "end Main; end C;"), false);
}

// §29.11 v0.338 consumption point 1: each terminal operator as the FIRST
// consumption spends the right; any later use is a compile error (#6197).
BEZEL_TEST(linq_single_consumption_1226,
           terminal_count_then_second_use_rejected_6197_spec_29_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var n := q.Count(); "
        "  var m := q.Count(); "
        "  return 0; "

        "end Main; end C;",
        {"bound lazy query `q` was already consumed", "§29.11", "terminal operator"}));
}

BEZEL_TEST(linq_single_consumption_1226,
           terminal_first_then_second_use_rejected_6197_spec_29_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var n := q.First(); "
        "  var m := q.Count(); "
        "  return 0; "

        "end Main; end C;",
        {"bound lazy query `q` was already consumed", "§29.11", "terminal operator"}));
}

BEZEL_TEST(linq_single_consumption_1226,
           terminal_toarray_then_second_use_rejected_6197_spec_29_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var n := q.ToArray(); "
        "  var m := q.Count(); "
        "  return 0; "

        "end Main; end C;",
        {"bound lazy query `q` was already consumed", "§29.11", "terminal operator"}));
}

BEZEL_TEST(linq_single_consumption_1226,
           terminal_sum_then_second_use_rejected_6197_spec_29_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var n := q.Sum(); "
        "  var m := q.Count(); "
        "  return 0; "

        "end Main; end C;",
        {"bound lazy query `q` was already consumed", "§29.11", "terminal operator"}));
}

// §29.11: a terminal op in one branch then a use after: mirrors the for-in
// rule (branch consumption is intersected); the conservative cases compile.
BEZEL_TEST(linq_single_consumption_1226,
           terminal_in_one_branch_then_use_after_mirrors_forin_6197_spec_29_11) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var n := 0; "
        "  if a.Count > 2 then n := q.Count(); else n := q.Count(); end; "
        "  return n + q.Count(); "
        "end Main; end C;"), true);
}

BEZEL_TEST(linq_single_consumption_1226,
           terminal_in_one_arm_only_then_use_after_conservative_6197_spec_29_11) {
    BEZEL_EXPECT_EQ(rejectedByDA(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var n := 0; "
        "  if a.Count > 2 then n := q.Count(); end; "
        "  return n + q.Count(); "
        "end Main; end C;"), false);
}

// §29.11 v0.338 consumption list: only "Enumeration or a terminal operator"
// spends the right; an Object member on the query (ToString, GetHashCode,
// Equals, GetType) is not on it, so a following single real terminal must not
// draw the consumed-binding diagnostic (#6197 follow-up). Whether the Object
// member itself compiles is a separate matter (#2457), so these assert only the
// ABSENCE of the consumed text.
BEZEL_TEST(linq_single_consumption_1226,
           object_member_tostring_before_terminal_not_consumption_6197_spec_29_11) {
    BEZEL_EXPECT_EQ(daErrorContains(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var o := q.ToString(); "
        "  var n := q.Count(); "
        "  "
        "  return n; "
        "end Main; end C;",
        "was already consumed"), false);
}

BEZEL_TEST(linq_single_consumption_1226,
           object_member_tostring_after_terminal_not_consumption_6197_spec_29_11) {
    BEZEL_EXPECT_EQ(daErrorContains(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  "
        "  var n := q.Count(); "
        "  var o := q.ToString(); "
        "  return n; "
        "end Main; end C;",
        "was already consumed"), false);
}

BEZEL_TEST(linq_single_consumption_1226,
           object_member_gethashcode_before_terminal_not_consumption_6197_spec_29_11) {
    BEZEL_EXPECT_EQ(daErrorContains(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var o := q.GetHashCode(); "
        "  var n := q.Count(); "
        "  "
        "  return n; "
        "end Main; end C;",
        "was already consumed"), false);
}

BEZEL_TEST(linq_single_consumption_1226,
           object_member_gethashcode_after_terminal_not_consumption_6197_spec_29_11) {
    BEZEL_EXPECT_EQ(daErrorContains(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  "
        "  var n := q.Count(); "
        "  var o := q.GetHashCode(); "
        "  return n; "
        "end Main; end C;",
        "was already consumed"), false);
}

BEZEL_TEST(linq_single_consumption_1226,
           object_member_gettype_before_terminal_not_consumption_6197_spec_29_11) {
    BEZEL_EXPECT_EQ(daErrorContains(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  var o := q.GetType(); "
        "  var n := q.Count(); "
        "  "
        "  return n; "
        "end Main; end C;",
        "was already consumed"), false);
}

BEZEL_TEST(linq_single_consumption_1226,
           object_member_gettype_after_terminal_not_consumption_6197_spec_29_11) {
    BEZEL_EXPECT_EQ(daErrorContains(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var q := a.Where(lambda(x) => x > 1); "
        "  "
        "  var n := q.Count(); "
        "  var o := q.GetType(); "
        "  return n; "
        "end Main; end C;",
        "was already consumed"), false);
}

