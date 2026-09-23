// Spec-derived regression test for rule-corpus `s17_7_0003` (spec §17.7 /
// §15.1, v0.307).
//
// THE GOVERNING RULE. Spec §17.7 "The `new` operator" (v0.307), verbatim:
// "`new` is not used to construct inline arrays. Inline arrays (`[N]T`,
// §15.1) are constructed by collection literals (`[expr, expr, ...]` ...) or
// by the implicit zero/null fill at declaration ... `new [3]int(0, 1, 2)` is
// rejected by the type checker — inline arrays do not declare constructors,
// and the `new` syntax has no meaning on them."
//
// ROOT CAUSE. Every `new`-classification predicate in
// src/sema/ExprTypecheck_Exprs.cpp (`rejectNewOnEnumOrChoice`,
// `rejectNewOnNonInstantiableKind`, `checkBuiltinClosedConstructorSurface`)
// guards on `rt->kind == TypeRef::Kind::Named`. The parser accepts
// `new [3]int(0, 1, 2)` — `parseType()` happily parses `[3]int` as an
// inline-array type spelling after `new` (Parser_Exprs.cpp) — and the
// resolved TypeRef's kind is `TypeRef::Kind::InlineArray`, a kind none of
// those predicates recognize. The NewExpr-typing arm therefore typed the
// expression as `[3]int` with no rejection at all, silently discarding the
// constructor arguments.
//
// MEASURED on `bf7a7515` (this lane's base, `r19-c20`) with `build/bezelc`:
// `var a := new [3]int(0, 1, 2);` compiled with rc=0 (the only diagnostic
// present was the unrelated §28.3 auto-drop must-use warning on the unused
// local `a`).
//
// THE FIX. `rejectNewOnInlineArray` (ExprTypecheck_Exprs.cpp), a new sibling
// of `rejectNewOnEnumOrChoice`, wired at the explicit `new T(...)` arm AND
// all four §17.7 `new()` shorthand call sites (ExprTypecheck.cpp) so a
// shorthand spelling targeting an inline-array declared type cannot slip
// through the same gap either.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// rule-corpus `s17_7_0003`, verbatim shape: `new [3]int(0, 1, 2)` at a typed
// local declaration.
const char* kNewOnInlineArray =
    "module Conf; "
    "public class Program "
    "  public static method Main() "
    "    var a := new [3]int(0, 1, 2); "
    "  end Main; "
    "end Program;";

// The same shape reached through the §17.7 `new()` shorthand — a typed local
// declaration whose declared type is an inline array. Companion coverage:
// the shorthand call sites (field/local/param/return) must reach the same
// authority, not just the explicit-name arm.
const char* kNewShorthandOnInlineArray =
    "module Conf; "
    "public class Program "
    "  public static method Main() "
    "    var a: [3]int := new(); "
    "  end Main; "
    "end Program;";

}  // namespace

BEZEL_TEST(sema_new_inline_array_construction_rejected_s17_7_0003,
           explicit_new_on_inline_array_rejected) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(kNewOnInlineArray, "cannot construct an inline array"));
    BEZEL_EXPECT_TRUE(rejectsWith(kNewOnInlineArray, "\xC2\xA7""17.7"));
}

BEZEL_TEST(sema_new_inline_array_construction_rejected_s17_7_0003,
           new_shorthand_on_inline_array_target_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(kNewShorthandOnInlineArray,
                                   "cannot construct an inline array"));
}

// ACCEPT control: the legitimate spellings §17.7 names as the inline array's
// real constructors — a collection literal, and the implicit zero fill —
// must be entirely unaffected by the new rejection.
BEZEL_TEST(sema_new_inline_array_construction_rejected_s17_7_0003,
           collection_literal_inline_array_construction_still_accepted) {
    BEZEL_EXPECT_TRUE(runs(6,
        "module Conf; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: [3]int := [1, 2, 3]; "
        "    return a[0] + a[1] + a[2]; "
        "  end Main; "
        "end Program;"));
}

// ACCEPT control: `new` on an ordinary class is completely unaffected by the
// new InlineArray-kind guard (it must still route through the Named-kind
// checks exactly as before).
BEZEL_TEST(sema_new_inline_array_construction_rejected_s17_7_0003,
           new_on_ordinary_class_still_accepted) {
    BEZEL_EXPECT_TRUE(runs(1,
        "module Conf; "
        "public class Widget "
        "  public constructor() "
        "  end constructor; "
        "end Widget; "
        "public class Program "
        "  public static method Main(): int "
        "    var w := new Widget(); "
        "    _ := w; "
        "    return 1; "
        "  end Main; "
        "end Program;"));
}
