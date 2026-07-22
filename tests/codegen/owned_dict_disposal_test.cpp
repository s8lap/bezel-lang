// Codegen — scope-end disposal of an OwningDictionary<K,V>'s surviving values
// (issue #394, stdlib §4.9). Companion coverage for OwningSet<T> (stdlib §4.10),
// whose class elements are array-backed and already disposed via #362.
//
// stdlib §4.9: "Owning hash-based map. Owns its values; keys are value-like."
// The dictionary's `Dispose()` cascade handles every value disposal — there is
// no manual element pass (the borrowing `Dictionary<K, V>.DisposeElements()`
// escape hatch was retired with axiom A8, §28.11 v0.213, #2081; owning
// collections never had one). Keys are NOT owned ("`OwningDictionary` does not
// support owned keys"), so only the values (V, constrained `V: class`) are
// disposed at scope end.
//
// stdlib §4.10: "`OwningSet<T>` ... where T: class" — owns its elements; each
// surviving element is disposed once at scope end.
//
// The bug (#394): the scope-end `using var` disposal of an OwningDictionary was
// not wired at all — neither the backing hash table nor its surviving values
// were freed. #388 wired the SETTER's disposal of a DISPLACED value
// (bz_dict_set_*_disposing); this file covers the values still present in the
// table when the `using var` scope ends, which must be disposed exactly once.
//
// Two complementary check kinds (mirroring undisposed_leak_test.cpp):
//   * destructor-count JIT tests (always run): a resource class's destructor
//     bumps a static counter, observable through the int exit code — proves the
//     scope-end disposal now runs each surviving value's destructor exactly the
//     right number of times.
//   * LeakSanitizer native tests (run only with a sanitizer toolchain): lower
//     to an object file, link against an ASan-instrumented runtime, run under
//     LSan — proves the values' blocks are reclaimed with NO leak AND NO
//     double-free / use-after-free (load-bearing for the displaced+surviving
//     case, where the displaced value must NOT be disposed twice).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Compile with generated-code AddressSanitizer, run under LeakSanitizer, and
// return the program's exit code — the dispose-exactly-once oracle. A leak /
// double-free / use-after-free (the load-bearing checks for #394/#396/#1214)
// FAILS the run (ran=false -> -999), so the sanitizer verdict rides on the same
// assertion as the destructor-count value. This consolidates the retired
// hand-rolled JIT-count + bespoke-LSan pair (which lowered the program to an
// object file and linked -fsanitize=address by hand) into the one standard
// checked AOT pipeline.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Assert the program disposes each surviving value exactly once (no leak, no
// double-free, no use-after-free) AND returns `expectedExitCode` — both verdicts
// ride on the single checked run.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
}

// A resource class R (destructor bumps R._n) and a Main holding the given body.
std::string prog(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;";
}

// --- OwningDictionary<string, R> — string-key fast path (§4.9). -------------

// Two owned values added via .Add(k, move v); both survive to scope end. The
// dictionary's Dispose() cascade disposes both values exactly once. Encoded:
// <count inside the scope, before exit> * 10 + <count after scope end>. Inside
// the scope nothing is disposed (0); at scope end both are disposed (2) → 02.
// Before #394 the values leaked and the count stayed 0 → 00.
const char* kDictStrTwoSurvivors =
    "var inside := -1; "
    "do "
    "  using var d := new OwningDictionary<string, R>(); "
    "  d.Add(\"a\", move (new R())); "
    "  d.Add(\"b\", move (new R())); "
    "  inside := R._n; "
    "end; "
    "return inside * 10 + R._n;";

// The displaced + surviving combination (the no-double-free case). Three keys
// are written via the indexer setter (§4.9 set(owned)); key "a" is written
// twice, so its first value is DISPLACED and disposed by the #388 setter path.
// At the displacement point R._n == 1. At scope end the THREE surviving values
// ("a"-second, "b", "c") are disposed → R._n == 4 total. The displaced value is
// already gone from the table, so it is NOT disposed again. Encoded:
// <count after displacement> * 10 + <count after scope end> = 1*10 + 4 = 14.
// Before #394: only the displaced one was disposed → 1*10 + 1 = 11.
const char* kDictStrDisplacedPlusSurvivors =
    "var afterDisplace := -1; "
    "do "
    "  using var d := new OwningDictionary<string, R>(); "
    "  d[\"a\"] := move (new R()); "
    "  d[\"b\"] := move (new R()); "
    "  d[\"c\"] := move (new R()); "
    "  d[\"a\"] := move (new R()); "   // displaces "a"-first (disposed here)
    "  afterDisplace := R._n; "
    "end; "
    "return afterDisplace * 10 + R._n;";

// --- OwningDictionary<int, R> — generic-key path (§4.9 / D3). ---------------

// An int key uses the generic-key runtime (bz_dict_*_gen). The owned values are
// still disposed at scope end. Two survivors → 02 (0 inside, 2 after).
const char* kDictIntTwoSurvivors =
    "var inside := -1; "
    "do "
    "  using var d := new OwningDictionary<int, R>(); "
    "  d.Add(1, move (new R())); "
    "  d.Add(2, move (new R())); "
    "  inside := R._n; "
    "end; "
    "return inside * 10 + R._n;";

// --- OwningSet<R> — array-backed class elements (§4.10, already via #362). ---

// OwningSet<T> where T: class is array-backed (a class element is not a generic-
// key-eligible value type), so its scope-end disposal already cascades into each
// element via the OwningArray element-disposer (#362). This test pins that
// behavior so the dict fix does not regress it. Two elements → 02.
const char* kSetTwoSurvivors =
    "var inside := -1; "
    "do "
    "  using var s := new OwningSet<R>(); "
    "  s.Add(move (new R())); "
    "  s.Add(move (new R())); "
    "  inside := R._n; "
    "end; "
    "return inside * 10 + R._n;";

// --- #396: explicit .Dispose() on an OwningDictionary disposes its values. ---

// #396 / §4.9 + §28.2: an explicit `d.Dispose()` on an OwningDictionary<K,V>
// (V a class) must cascade disposal into its surviving values — exactly as the
// scope-end (#394) path does. Here both values are disposed BY the explicit
// call (count 2 read AFTER it, INSIDE the scope), and the using-var's scope-end
// disposal is a NO-OP (§28.2 idempotency: the first Dispose disposed; the
// second is skipped), so the count is still 2 after the block → 2*10 + 2 = 22.
//
// This is the exactly-once interaction the issue calls out: explicit Dispose
// followed by scope-end disposal of the same `using var`. Before #396 the
// explicit call passed a NULL value-disposer (values leaked → inside count 0)
// AND then bz_dict_free ran a SECOND time on the freed table at scope end (a
// double-free / crash). The fix disposes the values once and nulls the slot so
// the scope-end bz_dict_free reloads null and no-ops. The LSan leg is the
// load-bearing check: no leak, no double-free, no use-after-free.
const char* kDictExplicitDispose =
    "var inside := -1; "
    "do "
    "  using var d := new OwningDictionary<string, R>(); "
    "  d.Add(\"a\", move (new R())); "
    "  d.Add(\"b\", move (new R())); "
    "  d.Dispose(); "
    "  inside := R._n; "          // both disposed by the explicit call → 2
    "end; "                       // scope-end disposal is a no-op (slot nulled)
    "return inside * 10 + R._n;"; // 2*10 + 2 = 22

// #396 / §4.9 + §28.4/§28.5: an owned `OwningDictionary` FIELD is auto-disposed
// by the destructor's field-disposal pass, which cascades into the dict's
// surviving values. The pass runs AFTER the (here empty) destructor body, so no
// explicit call is needed — indeed an explicit `this._d.Dispose()` in the
// destructor is a compile error (§28.5), because the generated Dispose() runs
// the body and THEN auto-disposes the field, so a hand call would double-free.
// The owner is a `using var`; at its scope end the field-disposal pass disposes
// the dict's one value exactly once (R._n → 1). (The pre-migration form called
// `this._d.Dispose()` by hand on the stale premise that hash-backed dict fields
// were not auto-disposed; that premise is fixed, and the JIT harness ran the
// now-rejected double-free shape only because it skips the ownership pass.)
const char* kDictFieldDestructorDispose =
    "module M; "
    "class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; "
    "class Owner "
    "  private owned _d: OwningDictionary<string, R>; "
    "  public constructor() "
    "    this._d := new OwningDictionary<string, R>(); "
    "    this._d.Add(\"a\", move (new R())); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Owner; "
    "class C public static method Main(): int "
    "  do using var o := new Owner(); end; "
    "  return R._n; "
    "end Main; end C;";

}  // namespace

// ---- OwningDictionary scope-end disposal (§4.9, #394) ---------------------

BEZEL_TEST(codegen_owned_dict_394, str_key_surviving_values_disposed_4_9) {
    // Both surviving values disposed once at scope end (none disposed inside) →
    // 0*10 + 2 = 2. Before #394 the values leaked → 0. LSan leg: no leak.
    expectDisposedAndLeakFree(prog(kDictStrTwoSurvivors), 2);
}

BEZEL_TEST(codegen_owned_dict_394, str_key_displaced_plus_surviving_no_double_free_4_9) {
    // #388 setter disposes the displaced "a"-first value (count 1 at the
    // displacement point); the three survivors are disposed at scope end
    // (count 4 total). The displaced value is NOT disposed twice → 1*10 + 4 =
    // 14. Before #394 the survivors leaked → 1*10 + 1 = 11. The LSan leg is the
    // load-bearing check that there is no double-free of the displaced value.
    expectDisposedAndLeakFree(prog(kDictStrDisplacedPlusSurvivors), 14);
}

BEZEL_TEST(codegen_owned_dict_394, int_key_surviving_values_disposed_4_9) {
    // Generic-key dictionary: the surviving owned values are disposed at scope
    // end exactly like the string-key path → 0*10 + 2 = 2.
    expectDisposedAndLeakFree(prog(kDictIntTwoSurvivors), 2);
}

// ---- OwningSet scope-end disposal (§4.10) — regression pin ----------------

BEZEL_TEST(codegen_owned_set_394, class_elements_surviving_disposed_4_10) {
    // OwningSet<R> (R a class) is array-backed; its surviving elements are
    // disposed at scope end via the OwningArray element-disposer (#362). This
    // must remain true after the dict fix → 0*10 + 2 = 2.
    expectDisposedAndLeakFree(prog(kSetTwoSurvivors), 2);
}

// ---- #396: explicit .Dispose() disposes OwningDictionary values (§4.9, §28.2) -

BEZEL_TEST(codegen_owned_dict_396, explicit_dispose_disposes_values_exactly_once_4_9) {
    // Explicit d.Dispose() disposes both values (inside count 2); the using
    // var's scope-end disposal is a §28.2 no-op (slot nulled), so the count is
    // still 2 after the block → 22. Before #396: values leaked AND the table
    // was double-freed at scope end. The LSan leg is load-bearing — it confirms
    // exactly-once disposal with no double-free / use-after-free.
    expectDisposedAndLeakFree(prog(kDictExplicitDispose), 22);
}

BEZEL_TEST(codegen_owned_dict_396, owned_dict_field_auto_disposed_by_destructor_4_9) {
    // The destructor's §28.4 field-disposal pass auto-disposes the owned dict
    // field, cascading into its one value exactly once → R._n == 1. The checked
    // run confirms no leak and no double-free (an explicit hand-Dispose of the
    // field would double-free and is a compile error, §28.5).
    expectDisposedAndLeakFree(kDictFieldDestructorDispose, 1);
}

// ---- #1214: OwningDictionary.Add disposes the rejected value on dup-key throw -
//
// §4.8 / §28.3: when OwningDictionary.Add is called with a key that is already
// present it must (a) throw ArgumentException and (b) dispose the transferred
// owned value — ownership was transferred to the call site and the value will
// not be inserted, so the disposer must run on the throw path or the value leaks.
//
// Encoding: `inside_throw * 10 + R._n` where `inside_throw` is R._n captured
// inside the catch arm (≥1 iff the rejected value was disposed on the throw
// path) and `R._n` is the final count after the using-var scope ends.
//
// Correct outcome: inside_throw == 1 (rejected R disposed), R._n == 2 (surviving
// first R disposed at scope end) → 12.
// Before #1214: the rejected R leaked, inside_throw == 0 → return 2 or crash.
// The LSan leg is load-bearing: it confirms exactly-once disposal with no leak.

const char* kOwnedDictStrDupKeyDisposesValue =
    "var inside_throw := -1; "
    "do "
    "  using var d := new OwningDictionary<string, R>(); "
    "  d.Add(\"a\", move (new R())); "
    "  try d.Add(\"a\", move (new R())); "
    "  catch ArgumentException => inside_throw := R._n; end; "
    "end; "
    "return inside_throw * 10 + R._n;";

// Same scenario with a generic (non-string) int key — exercises the
// bz_dict_contains_gen probe path instead of bz_dict_contains_str.
const char* kOwnedDictIntDupKeyDisposesValue =
    "var inside_throw := -1; "
    "do "
    "  using var d := new OwningDictionary<int, R>(); "
    "  d.Add(1, move (new R())); "
    "  try d.Add(1, move (new R())); "
    "  catch ArgumentException => inside_throw := R._n; end; "
    "end; "
    "return inside_throw * 10 + R._n;";

BEZEL_TEST(codegen_owned_dict_1214,
           str_key_dup_add_disposes_rejected_value_spec_4_8_28_3) {
    // String key: the rejected second R is disposed on the throw path
    // (inside_throw == 1 inside the catch), the surviving first R is disposed
    // at scope end (R._n == 2 after the block) → 12.
    // LSan leg: no leak (rejected value freed), no double-free (surviving value
    // freed exactly once at scope end).
    expectDisposedAndLeakFree(prog(kOwnedDictStrDupKeyDisposesValue), 12);
}

BEZEL_TEST(codegen_owned_dict_1214,
           int_key_dup_add_disposes_rejected_value_spec_4_8_28_3) {
    // Generic int key: same ownership guarantee via the bz_dict_contains_gen
    // probe path → inside_throw == 1, R._n == 2 after scope → 12.
    // LSan leg: load-bearing for the generic-key probe path.
    expectDisposedAndLeakFree(prog(kOwnedDictIntDupKeyDisposesValue), 12);
}
