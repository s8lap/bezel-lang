// Codegen — fix #3768 / stdlib §4.8 / §28.9 (v0.286): `lowerDictValuesFor`
// (`LlvmBackend_Stmts.cpp`, `for var v in d.Values do ... end`) carried a
// SEVENTH, independent inline copy of the exact narrow value-decode predicate
// fix #3758 unified everywhere else — `if (valRec) {...} else if
// (valNullableVal && isStructTy()) {...} else if (!isPointerTy()) {...}` —
// and was NOT named by #3758's own audit of "the DictionaryEntry decode
// copies" because `.Values` iteration decodes only the value half (no
// `DictionaryEntry<K,V>` struct), so it fell outside that family's scope
// entirely. A struct-lowering V that is neither a record nor a nullable
// value type — i.e. a `choice` — hit the scalar arm's `trunc i64 to
// <struct>`, which the LLVM IR verifier rejects: `for var v in d.Values`
// over a `Dictionary<int, Choice>` crashed the COMPILER (`Trunc only
// produces integer`), on both the #3758 PARENT and the #3758 FIX commit —
// #3758's own routing of `decodeDictValueAt` through the shared predicate
// never touched this SEPARATE copy.
//
// FIX. `lowerDictValuesFor`'s decode now routes through `decodeDictValueAt`
// (the SAME helper `lowerDictEntryFor`'s for-in loop already used, per
// #3758) instead of re-deriving its own copy — struct-load a boxed slot per
// `dictValueIsBoxedStruct` (LlvmBackend_impl.hpp, shared with
// `dictValueBoxFreer`, #3770), else undo the ptrtoint scalar encoding, else
// pass the raw pointer through as a borrow. `isStructTy()` is a strict
// superset of the old inline predicate for every V this loop is reachable
// with, so int / nullable-int / record / class / string V all decode
// IDENTICALLY to before (verified below, one regression row per kind); only
// a struct-lowering V that is neither record nor nullable-value — a choice —
// is newly admitted.
//
// FAIL-WITHOUT / PASS-WITH (measured against this lane's rebase, 89eb914):
// built a PARENT bezelc via `git archive HEAD` into a separate scratch tree.
// The issue's own repro (`for var v in d.Values do n := n + 1; end;` over a
// `Dictionary<int, Shape>`, Shape a choice) fails `bezelc -c` on the PARENT
// with:
//   Trunc only produces integer
//   %NN = trunc i64 %NN to %choice.Shape
//   bezelc: codegen error: IR verification failed
// and compiles + runs correctly on the FIX.
//
// This loop decodes ONLY the values (never the boxes' lifetime) — freeing a
// choice V's box is `dictValueBoxFreer`'s job (#3770, a DIFFERENT function),
// exercised at the dictionary's own scope-end disposal. Every row below still
// disposes its dictionary (`using var`) so the strict `runAotChecked` oracle
// also proves #3770's fix reaches a dictionary read via `.Values` with no
// leak / double-free / UAF, not just a for-in-drained one.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// #3733 toolchain-SKIP burn-down. This file's 7 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is still recognised as an environment condition rather than a
// product failure, but it now MARKS THE ROW FAILED instead of hiding it.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

}  // namespace

// ---------------------------------------------------------------------------
// The issue's own repro shape, widened to a 200x generated-code-ASan loop and
// TWO cases (int-payload choice) so the assertion is on the DECODED VALUES,
// not just "did not crash". FAILS-WITHOUT (IR verifier: `Trunc only produces
// integer`) / PASSES-WITH.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_values_choice_3768, int_payload_choice_value_decodes_4_8) {
    const std::string src =
        "module M; "
        "choice Shape Circle(r: int), Square(s: int); end Shape; "
        "class C public static method Main(): int "
        "  var total := 0; var n := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Shape>(); "
        "    d.Add(1, Shape.Circle(3)); "
        "    d.Add(2, Shape.Square(4)); "
        "    for var v in d.Values do "
        "      n := n + 1; "
        "      total := total + (case v of Circle(var r) => r, Square(var s) => s); "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"n={n} total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=400"));        // 200 x 2 entries
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));   // 200 x (3 + 4)
}

// ---------------------------------------------------------------------------
// STRING-payload choice value: a choice case carrying an OWNED `string`
// field, proving the struct-load path neither double-releases it nor
// use-after-frees it across 200 full Add-through-dispose cycles under the
// STRICT oracle (unlike #3758's sibling row, this one was never gated by the
// #3770 box-leak blocker — `.Values` iteration alone never reached the
// crashing predicate copy before this fix existed to leak from, and this row
// is written post-#3770 so it runs strict from the start).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_values_choice_3768, string_payload_choice_value_no_uaf_28_9) {
    const std::string src =
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Msg>(); "
        "    d.Add(1, Msg.Note(\"ab\")); "
        "    d.Add(2, Msg.Note(\"cde\")); "
        "    for var v in d.Values do "
        "      total := total + (case v of Note(var t) => t.Length, Empty => 0); "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));  // 200 x (2 + 3)
}

// ---------------------------------------------------------------------------
// Object-parity regressions, one row per V kind `lowerDictValuesFor` already
// handled before this fix — the refactor swapped its inline decode for a call
// through `decodeDictValueAt`, so each of these pins that the swap changed
// NOTHING for the kinds that already worked.
// ---------------------------------------------------------------------------

// int V — the scalar (non-struct, non-pointer) ptrtoint/trunc-back arm.
BEZEL_TEST(dict_values_choice_3768, int_value_regression_4_8) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, int>(); "
        "    d.Add(1, 10); d.Add(2, 20); "
        "    for var v in d.Values do total := total + v; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=6000"));  // 200 x (10 + 20)
}

// nullable-int V (int?) — the struct-load arm via the OTHER pre-existing
// boxed shape (issue #539's tagged {i8,T} struct), unchanged by this fix.
BEZEL_TEST(dict_values_choice_3768, nullable_int_value_regression_539) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, int?>(); "
        "    d.Add(1, 10); d.Add(2, null); "
        "    for var v in d.Values do "
        "      total := total + (if v <> null then v! else -1); "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1800"));  // 200 x (10 + -1)
}

// record V — the OTHER pre-existing struct-load arm (#563), unchanged.
BEZEL_TEST(dict_values_choice_3768, record_value_regression_563) {
    const std::string src =
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, Pt>(); "
        "    d.Add(1, new Pt(3, 4)); "
        "    for var v in d.Values do total := total + v.x + v.y; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x (3 + 4)
}

// string V — the pointer (borrow-of-shared-reference) arm, unchanged.
BEZEL_TEST(dict_values_choice_3768, string_value_regression_4_8) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var d := new Dictionary<int, string>(); "
        "    d.Add(1, \"ab\"); d.Add(2, \"cde\"); "
        "    for var v in d.Values do total := total + v.Length; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));  // 200 x (2 + 3)
}

// class V — the pointer (borrow) arm over a class-typed value, unchanged.
BEZEL_TEST(dict_values_choice_3768, class_value_regression_4_8) {
    const std::string src =
        "module M; "
        "class R public v: int; public constructor(v: int) this.v := v; end constructor; end R; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    using var r1 := new R(3); using var r2 := new R(4); "
        "    var d := new Dictionary<int, R>(); "
        "    d.Add(1, r1); d.Add(2, r2); "
        "    for var v in d.Values do total := total + v.v; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"total={total}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1400"));  // 200 x (3 + 4)
}
