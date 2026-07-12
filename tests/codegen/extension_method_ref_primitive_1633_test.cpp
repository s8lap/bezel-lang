// fix #1633 / §26.5 / §27.6: a type-qualified extension-method reference on a
// PRIMITIVE receiver — `int.Doubled` used as an `(int) -> int` value — must
// resolve and lower correctly. §27.6: "For an extension method, the reference
// uses the extended type's name as qualifier; the resulting function has the
// receiver as its first parameter." The RECORD-receiver form of this same
// reference shape (`Vec.Scaled`) was fixed by #1306
// (extension_method_ref_record_1306_test.cpp); this file covers the
// PRIMITIVE-receiver form, which #1306 explicitly left out of scope (found
// while fixing #1306 — see the issue).
//
// BUG (#1633): sema's primitive-keyword-qualifier member-access path (entered
// whenever the qualifier names a primitive type, e.g. `int`, `double`, `char`
// — such a qualifier has NO TypeSymbol, hence no ordinary member lookup) never
// consulted extension-method lookup before rejecting an unrecognized member
// name. It recognized only a fixed, closed set of builtin static names
// (Parse, MaxValue, …) and any other name — including a real user-declared
// extension method — fell straight to a "type 'int' has no static method or
// property 'Doubled'" (§3.4) diagnostic. The instance-receiver form
// (`x.Doubled()`, x: int) was unaffected — it resolves through the ordinary
// §26.5 extension-lookup path, which already tries extensions.
//
// Two-layer fix (mirroring #1306's rework, 191b71d):
//  * sema (resolveMemberAccess primitive-keyword block): try
//    lookupExtensionMember on the primitive's TypeRef before the catch-all
//    diagnostic (mirrors the existing string-instance path, #702, and the
//    ordinary Named-receiver path).
//  * sema (resolveExtensionMember): the UNBOUND-vs-BOUND detection that
//    #1306's rework added keys off the qualifier's exprSymbol being a
//    TypeSymbol — but a primitive keyword qualifier (`int`) has NO exprSymbol
//    at all (§5.2: "it has no value symbol; the enclosing call resolves the
//    conversion"), so that detection never fired for a primitive qualifier.
//    Extended it to also recognize a primitive-keyword identifier in
//    qualifier position as unbound (no local/parameter can be named `int`,
//    a reserved word, so this qualifier shape is never a value).
//
// These tests AOT-run programs that take a type-qualified extension-method
// reference on a primitive and call it; without the fix they fail to compile
// (-999).

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

// §27.6 (#1633): `int.Doubled` taken as an `(int) -> int` value and called —
// the core repro from the issue. Without the fix: compile error (-999),
// "type 'int' has no static method or property 'Doubled'". Expected 42
// (Doubled(21) = 42).
BEZEL_TEST(codegen_extension_method_ref_primitive_1633, doubled_int_typequal_ref_1633) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Doubled(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> int := int.Doubled; "
        "    return f(21); "
        "  end Main; "
        "end C;"), 42);
}

// §27.6: the reference's signature carries the receiver PLUS the method's
// explicit parameters — `int.Plus(n)` is `(int, int) -> int`. Verifies the
// thunk forwards both the primitive receiver (passed by value, unlike a
// record's spilled pointer) and the explicit argument.
BEZEL_TEST(codegen_extension_method_ref_primitive_1633, typequal_ref_with_explicit_param) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Plus(n: int): int => this + n; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int, int) -> int := int.Plus; "
        "    return f(20, 22); "  // 20 + 22 = 42
        "  end Main; "
        "end C;"), 42);
}

// §27.6 / §27.7: the function value is callable multiple times with
// different receivers.
BEZEL_TEST(codegen_extension_method_ref_primitive_1633, typequal_ref_multiple_calls) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Doubled(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> int := int.Doubled; "
        "    var sum := f(10); "   // 20
        "    sum := sum + f(11); " // 22
        "    return sum; "         // 42
        "  end Main; "
        "end C;"), 42);
}

// Regression (§26.5, #457): a BOUND primitive extension-method reference
// (instance qualifier, `x.Doubled`) must still capture its receiver — the
// case that already worked. Guards against the #1633 unboundExtRef detection
// change misclassifying a real primitive-VALUE qualifier as unbound.
BEZEL_TEST(codegen_extension_method_ref_primitive_1633, bound_instance_ref_regression) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Doubled(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var x := 21; "
        "    var f: () -> int := x.Doubled; "
        "    return f(); "
        "  end Main; "
        "end C;"), 42);
}

// Regression: a DIRECT extension call on a primitive (`x.Doubled()`) — the
// #1633 note isolates the bug to the type-qualified-reference path — must
// still work, and calling the method directly via the type name would be a
// separate (non-reference) call form that this test also covers for
// completeness against the primitive-static resolution path this fix touches.
BEZEL_TEST(codegen_extension_method_ref_primitive_1633, direct_primitive_extension_call_regression) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Doubled(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var x := 21; "
        "    return x.Doubled(); "  // 42
        "  end Main; "
        "end C;"), 42);
}

// Regression: an unrecognized member on a primitive-keyword qualifier must
// still be a clean §3.4 diagnostic (compile error), not a silent accept —
// the extension lookup added by #1633 must not swallow genuinely unknown
// names. `int.Bogus` names neither a builtin static member nor any
// user-declared extension, so the call must fail to compile.
BEZEL_TEST(codegen_extension_method_ref_primitive_1633, unknown_primitive_static_still_rejected) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> int := int.Bogus; "
        "    return f(1); "
        "  end Main; "
        "end C;"), -999);
}
