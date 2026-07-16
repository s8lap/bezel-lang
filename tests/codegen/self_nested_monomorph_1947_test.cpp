// #1947 (§14.2 monomorphization / §12.9 invariance / §19.3 v0.182 / §19.6
// v0.196) — self-nested generic instantiations must NOT collide in the
// monomorphization cache. When BOTH `Foo<int>` and `Foo<Foo<int>>` (a
// self-nesting of the SAME generic) are used as the type argument of one generic
// method, the specialized-body cache (buildSpecializedMethodFn, Calls2.cpp) keyed
// each on the top-level DECLARATION only (`typeRefIdentityToken` → the `Foo`
// declaration pointer). `Foo<int>` and `Foo<Foo<int>>` share that pointer AND
// both erase to `ptr` in IR, so the second instantiation reused the FIRST's
// specialized body — and a per-instantiation type test inside (`is Foo<T>` or
// `case o of T`) baked the first T's descriptor, silently mis-dispatching. The
// symptom is instantiation-ORDER dependent (first-seen wins): swapping the two
// call sites moved the wrong answer to the other one.
//
// The fix keys the cache on the RECURSIVE structural encoding (typeRefIdentityKey
// → appendDescKey) — the same encoding the runtime `is`/`as` descriptor honors —
// so every nested type argument participates and self-nested instantiations get
// DISTINCT specialized bodies.
//
// This also fixes #1944: a built-in-exception type argument carries
// `declaration == null`, so the OLD token was `nullptr` for BOTH `T=IOException`
// and `T=ArgumentException` and the two instantiations collapsed onto one body.
// The recursive key encodes a declaration-less Named type by its head name, so
// they are now distinguished.
//
// Each test observes values through the process exit code (< 256). Two-arm cases
// encode several booleans into one code. Without the fix EVERY multi-instantiation
// case below returns the wrong (collapsed) code; with it, the spec-stated value.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A generic `Wrap<T>` plus a helper preamble reused by the `is` cases. The
// method-level generic `Check<T>` has a BLOCK body, so it is monomorphized via
// buildSpecializedMethodFn (an EXPRESSION body would be inlined per call site and
// never exercise the cache — the very reason the defect hid).
const char* kWrapCheck =
    "module M; "
    "class Wrap<T> public V: T; "
    "  public constructor(v: T) V := v; end constructor; end Wrap; "
    "class C "
    "  public static method Check<T>(o: Object): int "
    "    if o is Wrap<T> then return 1; else return 0; end; "
    "  end Check; ";
}  // namespace

// The exact reported defect (`is Wrap<T>` surface): Check<Wrap<int>> and
// Check<Wrap<Wrap<int>>> must dispatch to DISTINCT bodies. A Wrap<Wrap<int>>
// value is a Wrap<Wrap<int>> (matches Check<Wrap<int>>'s `is Wrap<Wrap<int>>`);
// a Wrap<Wrap<Wrap<int>>> value matches Check<Wrap<Wrap<int>>>'s
// `is Wrap<Wrap<Wrap<int>>>`. Both must be 1 → 1*2 + 1 = 3. The collision
// returned 2 (the second call reused the first's `is Wrap<Wrap<int>>` body).
BEZEL_TEST(codegen_1947, is_self_nested_distinct_bodies) {
    BEZEL_EXPECT_EQ(run(std::string(kWrapCheck) +
        "  public static method Main(): int "
        "    using var wwi: Wrap<Wrap<int>> := new Wrap<Wrap<int>>(new Wrap<int>(1)); "
        "    using var wwwi: Wrap<Wrap<Wrap<int>>> := "
        "        new Wrap<Wrap<Wrap<int>>>(new Wrap<Wrap<int>>(new Wrap<int>(2))); "
        "    var owwi: Object := wwi; "
        "    var owwwi: Object := wwwi; "
        "    var a := Check<Wrap<int>>(owwi); "        // is Wrap<Wrap<int>>? → 1
        "    var b := Check<Wrap<Wrap<int>>>(owwwi); "  // is Wrap<Wrap<Wrap<int>>>? → 1
        "    return a * 2 + b; "                        // want 3; collision → 2
        "  end Main; end C;"), 3);
}

// Instantiation-ORDER independence: swap the two calls. The correct answer is
// still 3 — before the fix this returned 1 (the wrong answer moved to `a`),
// pinning the first-seen-wins nature of the collision.
BEZEL_TEST(codegen_1947, is_self_nested_order_independent) {
    BEZEL_EXPECT_EQ(run(std::string(kWrapCheck) +
        "  public static method Main(): int "
        "    using var wwi: Wrap<Wrap<int>> := new Wrap<Wrap<int>>(new Wrap<int>(1)); "
        "    using var wwwi: Wrap<Wrap<Wrap<int>>> := "
        "        new Wrap<Wrap<Wrap<int>>>(new Wrap<Wrap<int>>(new Wrap<int>(2))); "
        "    var owwi: Object := wwi; "
        "    var owwwi: Object := wwwi; "
        "    var b := Check<Wrap<Wrap<int>>>(owwwi); "  // FIRST now → 1
        "    var a := Check<Wrap<int>>(owwi); "         // SECOND → 1
        "    return a * 2 + b; "                        // want 3; collision → 1
        "  end Main; end C;"), 3);
}

// The `case o of T` surface (widened by the #1852 rework): a method-level generic
// `Kind<T>` with `case o of T`. Kind<Wrap<int>> and Kind<Wrap<Wrap<int>>> tested
// against a Wrap<int> value AND a Wrap<Wrap<int>> value:
//   a = Kind<Wrap<int>>(wi)        → Wrap<int> is Wrap<int>            → 2
//   b = Kind<Wrap<int>>(wwi)       → Wrap<Wrap<int>> is Wrap<int>      → 9
//   c = Kind<Wrap<Wrap<int>>>(wi)  → Wrap<int> is Wrap<Wrap<int>>      → 9
//   d = Kind<Wrap<Wrap<int>>>(wwi) → Wrap<Wrap<int>> is Wrap<Wrap<int>>→ 2
// Correct: (a,b,c,d) = (2,9,9,2) → 2*1000+9*100+9*10+2 = 2992 (176 mod 256).
// The collision made Kind<Wrap<Wrap<int>>> reuse the `is Wrap<int>` body, so
// (c,d) mirrored (a,b) = (2,9) → 2929 (113 mod 256).
BEZEL_TEST(codegen_1947, case_self_nested_per_instantiation) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrap<T> public V: T; "
        "  public constructor(v: T) V := v; end constructor; end Wrap; "
        "class C "
        "  public static method Kind<T>(o: Object): int "
        "    return case o of T => 2, _ => 9 end; "
        "  end Kind; "
        "  public static method Main(): int "
        "    using var wi: Wrap<int> := new Wrap<int>(1); "
        "    using var wwi: Wrap<Wrap<int>> := new Wrap<Wrap<int>>(new Wrap<int>(2)); "
        "    var owi: Object := wi; "
        "    var owwi: Object := wwi; "
        "    var a := Kind<Wrap<int>>(owi); "
        "    var b := Kind<Wrap<int>>(owwi); "
        "    var c := Kind<Wrap<Wrap<int>>>(owi); "
        "    var d := Kind<Wrap<Wrap<int>>>(owwi); "
        "    return a * 1000 + b * 100 + c * 10 + d; "  // want 2992 (176 mod 256)
        "  end Main; end C;"), 2992 % 256);
}

// #1944: built-in-exception type arguments (declaration == null) must also get
// distinct monomorphizations. ExBox<IOException> vs ExBox<ArgumentException>,
// each Kind()-testing a caught IOException and a caught ArgumentException:
//   io  = Probe(IOException)        → <IO>=2, <Arg>=9 → 29
//   arg = Probe(ArgumentException)  → <IO>=9, <Arg>=2 → 92
// Correct: io*100 + arg = 2992 (176 mod 256). The collapse (both bodies test the
// first-seen IOException) gave io=22, arg=99 → 2299 (251 mod 256).
BEZEL_TEST(codegen_1947, builtin_exception_type_args_distinct_1944) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ExBox<T> "
        "  public method Kind(e: Exception): int "
        "    return case e of T => 2, _ => 9 end; "
        "  end Kind; "
        "end ExBox; "
        "class Program "
        "  public static method Probe(e: Exception): int "
        "    using var eio := new ExBox<IOException>(); "
        "    using var earg := new ExBox<ArgumentException>(); "
        "    return eio.Kind(e) * 10 + earg.Kind(e); "
        "  end Probe; "
        "  public static method Main(): int "
        "    var io := 0; var arg := 0; "
        "    try throw new IOException(\"x\"); "
        "      catch e: Exception => io := Probe(e); end; "
        "    try throw new ArgumentException(\"a\"); "
        "      catch e: Exception => arg := Probe(e); end; "
        "    return io * 100 + arg; "  // want 2992 (176 mod 256); collapse → 2299
        "  end Main; end Program;"), 2992 % 256);
}

// Regression guard: a NON-nested pair and a DIFFERENT-generic pair are unaffected
// (the issue notes different-generic nesting was never broken). Check<int> tests
// `is Wrap<int>`; Check<string> tests `is Wrap<string>`. A Wrap<int> value:
// matches Check<int> (1), not Check<string> (0) → 1*2 + 0 = 2. This shares the
// SAME `Check` template but with primitive args (distinct tokens already), so it
// must remain correct after the key change.
BEZEL_TEST(codegen_1947, nonnested_primitive_args_unaffected) {
    BEZEL_EXPECT_EQ(run(std::string(kWrapCheck) +
        "  public static method Main(): int "
        "    using var wi: Wrap<int> := new Wrap<int>(7); "
        "    var owi: Object := wi; "
        "    var a := Check<int>(owi); "     // is Wrap<int>? on Wrap<int> → 1
        "    var b := Check<string>(owi); "  // is Wrap<string>? on Wrap<int> → 0
        "    return a * 2 + b; "             // 2
        "  end Main; end C;"), 2);
}
