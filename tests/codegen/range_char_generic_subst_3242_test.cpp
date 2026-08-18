// Codegen — issue #3242: Range<char> generic-substitution fallback.
// §8.5 component rule applied to a `Range<T>` whose T was substituted with
// `char` from a generic body must render Start/End via the char renderer
// (so `Range('a'..'c').ToString()` contains `Start: a`, not `Start: 97`).
//
// Bug: LlvmBackend_String.cpp's `emitRangeToString` derives the endpoint
// rendering kind from the supplied element TypeRef; when the range arrives
// through unresolved generic substitution (`Range<T>` whose T resolves to
// `char` only at the instantiation site), the supplied TypeRef is the bare
// `T` — not Char — and the fallback to `P::Long` produces a signed-64
// renderer. The output reads `Start: 97` instead of `Start: a`. The same
// substitution resolution (`resolveTypeParamRef`) the spec section's other
// code paths already apply (e.g. #2503, the interpolation-side fix,
// consults `genericSubstSemaTy_` for a bare T before deciding the renderer)
// was missing from `emitRangeToString`'s primitive-kind test.
//
// Fix: `emitRangeToString` now resolves its `elemTy` argument through
// `resolveTypeParamRef` (the same param-name lookup `resolveTypeParamRef`
// already exposes for every other codegen site that consults a bare-T ref
// for rendering kind). Centralizing the substitution in the renderer —
// rather than at each caller — covers all three call sites with one
// change: the §8.5 string-rendering arm just above (interpolation), the
// `Range<T>.ToString()` member-call arm in LlvmBackend_Exprs.cpp, and the
// box-vtable ToString thunk in LlvmBackend_Setup.cpp. Each was independently
// passing an unresolved `T` through to the primitive-kind test.
//
// §9.8 / §16.3 make `Range<char>` an ordinary (not exotic) instantiation —
// the fix must not regress the non-generic `Range('a'..'c')` shape or the
// integer generic-substitution case.
// §8.5 each component renders as its own `ToString` — virtual dispatch per
// component: a `char` range endpoint is a `char`, not an integer.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotOut — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Plain `module M; class C … Main` wrapper for programs that don't need
// their own wrapping.
std::string plain(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           "end Main; end C;";
}

}  // namespace

// ── The bug's own repro: a generic function that takes a `Range<T>`
//    parameter and renders its ToString. When T is instantiated to char,
//    the renderer must use the char renderer (Start: a), not the long
//    fallback (Start: 97). The bug was specifically about the
//    PASSED-IN-TO-a-generic shape — return-from-generic (`MakeRange<T>:
//    Range<T>`) was already working because sema carries the substituted
//    `Range<char>` for that direction.
//    FAILS before the fix (`Start: 97`); PASSES after (`Start: a`). ─────

BEZEL_TEST(range_char_generic_subst_3242,
           generic_param_range_char_renders_char) {
    const std::string program =
        "module M; class C "
        "  public static method Wrap<T>(r: Range<T>): string "
        "    return r.ToString(); "
        "  end Wrap; "
        "  public static method Main(): int "
        "    var s := Wrap('a'..'c'); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runAotOut(program),
        std::string("Range(Start: a, End: c, Step: 1, IsEndInclusive: True, "
                    "IsOpenEnded: False, IsEmpty: False)"));
}

// ── Companion: the same generic-substitution path with an integer T —
//    Range<int> substituted with `Range(1..3)` must render Start: 1 / End: 3
//    (signed-64 path, the existing default). Confirms the fix didn't swap
//    the integer rendering to char and didn't break the integer generic
//    path. ─────────────────────────────────────────────────────────────────

BEZEL_TEST(range_char_generic_subst_3242,
           generic_param_range_int_renders_signed_64) {
    const std::string program =
        "module M; class C "
        "  public static method Wrap<T>(r: Range<T>): string "
        "    return r.ToString(); "
        "  end Wrap; "
        "  public static method Main(): int "
        "    var s := Wrap(1..3); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runAotOut(program),
        std::string("Range(Start: 1, End: 3, Step: 1, IsEndInclusive: True, "
                    "IsOpenEnded: False, IsEmpty: False)"));
}

// ── Regression guard: the non-generic `Range('a'..'c')` form already
//    rendered chars before this fix and must continue to do so — the fix
//    routes only the substituted-through-generic case through the new
//    resolution; the concrete case is unaffected (the supplied TypeRef
//    already names Char). ───────────────────────────────────────────────────

BEZEL_TEST(range_char_generic_subst_3242,
           concrete_range_char_unaffected) {
    BEZEL_EXPECT_EQ(
        runAotOut(plain(
            "var r := 'a'..'c'; "
            "Console.WriteLine(r.ToString()); "
            "return 0; ")),
        std::string("Range(Start: a, End: c, Step: 1, IsEndInclusive: True, "
                    "IsOpenEnded: False, IsEmpty: False)"));
}

// ── Direction coverage: the generic-substitution shape with a
//    generic-range RETURNING function (the other direction that sema
//    already substitutes for) — must also render correctly, asserting
//    the fix doesn't break that path either. ───────────────────────────────

BEZEL_TEST(range_char_generic_subst_3242,
           generic_return_range_char_renders_char) {
    const std::string program =
        "module M; class C "
        "  public static method MakeRange<T>(lo: T, hi: T): Range<T> "
        "    return lo..hi; "
        "  end MakeRange; "
        "  public static method Main(): int "
        "    var r := MakeRange('a', 'c'); "
        "    Console.WriteLine(r.ToString()); "
        "    return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runAotOut(program),
        std::string("Range(Start: a, End: c, Step: 1, IsEndInclusive: True, "
                    "IsOpenEnded: False, IsEmpty: False)"));
}

// ── §18.5 interpolation hole: `$"{r}"` for a Range<T> substituted with
//    char reaches the same renderer through the §8.5 string-rendering arm
//    (the path the fix description cites first). Mirrors #2503's
//    interpolation-side coverage. ─────────────────────────────────────────

BEZEL_TEST(range_char_generic_subst_3242,
           generic_param_range_char_interpolation) {
    const std::string program =
        "module M; class C "
        "  public static method Wrap<T>(r: Range<T>): string "
        "    return $\"got {r}\"; "
        "  end Wrap; "
        "  public static method Main(): int "
        "    var s := Wrap('a'..'c'); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runAotOut(program),
        std::string("got Range(Start: a, End: c, Step: 1, "
                    "IsEndInclusive: True, IsOpenEnded: False, IsEmpty: False)"));
}
