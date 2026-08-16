// Spec-derived regression test for issue #3115.
//
// §3 / §5.1 / §8.6 (issue #3115, v0.255): an `Object`-typed source has
// NO implicit conversion to ANY non-`Object` value-typed or
// reference-typed target — the static "unbox" the prior checkAssignable
// admitted on `(!from->nullable || to->nullable)` had no runtime
// counterpart. Codegen's `lowerLocalDecl` (`src/codegen/LlvmBackend_Setup.cpp`)
// stores the `ptr`-typed slot at the value's storage and a later load
// reinterprets the box pointer as the value, so `var i: int := o;` prints
// the low 32 bits of the box's heap address under ASLR instead of `42`,
// and the assignment form `i := o;` produces the same garbage. The
// reference-typed-target case (`var s: string := o;`, DateTime, records,
// enums, choices, interfaces) is an unchecked downcast with no instance-of
// check, so a wrong-runtime-type target is instance confusion at the
// first member access / method call.
//
// The sanctioned spellings carry an explicit runtime check: §12.7's
// `is`-narrowing (`if o is T then var v: T := o; end;` — the proven-
// narrowing path #3090 leaves intact, unboxing correctly) or §17.4's
// `as`-cast (`var v := o as T;`, yielding `T?` so a failed unbox has
// somewhere to put `null`). A bare initializer does neither, and the
// only correct answer is a compile error.
//
// The two exceptions preserved by the fix are the `null → T?` shape
// (any nullable target admits the null literal — `var x: int? := null;`)
// and the `null → T` shape (which the existing §7 message already
// reports — the test `null_into_non_nullable_int_target_keeps_paragraph_7`
// pins).
//
// What this test pins:
//   * REJECT (LocalDecl position, value-typed target):
//       - `int`, `string` (reference-typed), `DateTime`, user record
//         (`Point`), user enum (`Color`), user choice (`Shape`),
//         interface-typed target (`IFoo` via class carrier).
//       - All seven primitive target kinds: `int`, `bool`, `char`,
//         `byte`, `long`, `decimal`, `string`. Each proves the gate
//         fires for Primitive-kind `to` regardless of width.
//   * REJECT (Assignment position): `i := o;` for an `int` target.
//   * REJECT (Return position): `return o;` for an `int` method.
//   * REJECT (Call-arg position): passing `o` to a `Foo(int)` parameter.
//     Proves the gate is NOT LocalDecl-only — a regression to a
//     LocalDecl-only fix would otherwise slip through.
//   * ACCEPT-CONTROLS: the legitimately-allowed conversions stay
//     admitted — `null → T?` (any nullable target), `null → T` (the
//     existing §7 diagnostic, NOT the new §5.1 message), `T → Object`
//     (boxing), direct primitive initialization, primitive widening.
//
// Each test asserts on diagnostic TEXT — `coarse rejects()` cannot
// distinguish a genuine compile error from a codegen ICE, and the §5.1
// diagnostic naming both remedies (§12.7 `is`, §17.4 `as`) is the
// load-bearing piece of the user-visible behavior.

#include <initializer_list>
#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The #3115 §5.1 diagnostic from checkAssignable (with the §12.7 + §17.4
// remedies spelled out). Every REJECT test below asserts on this fragment.
constexpr const char* kIssue3115Diag =
    "has type `Object` which has no implicit conversion";

// The existing §7 nullable-source diagnostic (preserved for the
// `null → T` case — keeps the right paragraph numbering).
constexpr const char* kParagraph7Nullable = "is nullable but the target type is non-nullable";

}  // namespace

// =====================================================================
// §3 / issue #3115 — REJECT: an `Object` initializer into a value-typed
// local. Pre-fix compiled clean and read the box pointer reinterpreted
// as the value. Diagnostic must point at the initializer, name §3/§5.1,
// and name BOTH sanctioned remedies (the §12.7 `is`-narrowing form and
// the §17.4 `as`-cast form).
// =====================================================================

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_int_local_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var i: int := o; "
        "    return i; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
    BEZEL_EXPECT_TRUE(hasError(r, "§12.7"));
    BEZEL_EXPECT_TRUE(hasError(r, "§17.4"));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_string_local_rejected_spec_3_5_1) {
    // `string` is a reference type per §3.1; the prior code admitted this
    // because both `from` and `to` are Primitive-kind (PrimitiveType::Object,
    // PrimitiveType::String) and the broad `(!from->nullable || to->nullable)`
    // early-return accepted. The Reference-typed-target half of the brief
    // ("also unsafe — unchecked downcast") makes this a reject; without a
    // runtime check, `o` could be any class instance, not a string.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := \"hi\"; "
        "    var s: string := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_datetime_local_rejected_spec_3_5_1) {
    // DateTime is a value-typed builtin (§2.5) — stored as i32, classified
    // as a value type per §3.2. Before the fix, `from` was Primitive(Object)
    // and `to` was a Named-with-null-declaration builtin, so neither the
    // Primitive-Primitive nor the Named-Named checkAssignable branch fired
    // and the call fell through to the trailing "Mixed / generic-parameter /
    // null-literal cases: skip." — silent accept, garbage read at runtime.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := DateTime.Now; "
        "    var dt: DateTime := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

// =====================================================================
// §3 / issue #3115 — REJECT: the assignment form (`i := o;`). The brief
// and the issue body name both shapes; the fix lives in checkAssignable
// so both the initializer position (LocalDecl) and the
// assignment-statement position reject the same way. Pre-fix: printed
// the box pointer reinterpreted as the value, same defect shape as the
// initializer.
// =====================================================================

BEZEL_TEST(sema_init_value_from_object_3115,
           object_assignment_into_int_local_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var i: int := 0; "
        "    i := o; "
        "    return i; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

// =====================================================================
// §3 / issue #3115 — ACCEPT-CONTROL: the legitimate `null → T?` path is
// preserved (it is the canonical initializer, `var x: int? := null;`).
// `null` is typed `Object?`; assigning to a nullable target is §5.1's
// "Nullable conversions" — no runtime check required because `null`
// has no payload to reinterpret. Both keyword `int?` and the
// named-target `string?` shapes are pinned.
// =====================================================================

BEZEL_TEST(sema_init_value_from_object_3115,
           null_into_nullable_int_target_accepted_spec_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x: int? := null; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kIssue3115Diag));
    BEZEL_EXPECT_TRUE(!hasError(r, kParagraph7Nullable));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           null_into_nullable_string_target_accepted_spec_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var s: string? := null; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kIssue3115Diag));
}

// =====================================================================
// §3 / issue #3115 — ACCEPT-CONTROL: `null → T` (non-nullable target)
// keeps the pre-existing §7 nullable-source diagnostic, not the new §5.1
// "not convertible" message. The user sees the §7 rule, which is the
// one that actually applies (`null` is not assignable to a non-nullable
// target — full stop). Without this guard the new code would mask the
// §7 message under a misleading §5.1 suggestion.
// =====================================================================

BEZEL_TEST(sema_init_value_from_object_3115,
           null_into_non_nullable_int_target_keeps_paragraph_7) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := null; "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kParagraph7Nullable));
    // The §5.1 fix's diagnostic is NOT the one the user sees here —
    // §7 owns the `null → non-nullable` shape and that rule is
    // preserved.
    BEZEL_EXPECT_TRUE(!hasError(r, kIssue3115Diag));
}

// =====================================================================
// §3 / issue #3115 — ACCEPT-CONTROL: integer → Object (boxing at every
// site — local-init, assignment, return) is unaffected. The early-return
// `to is Object → accept` at the top of checkAssignable still fires and
// is the symmetric direction of the fix (everything boxes, nothing
// unboxes implicitly).
// =====================================================================

BEZEL_TEST(sema_init_value_from_object_3115,
           int_into_object_local_accepted_boxing) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kIssue3115Diag));
    BEZEL_EXPECT_TRUE(!hasError(r, kParagraph7Nullable));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           int_literal_into_object_local_accepted_boxing) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 7; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kIssue3115Diag));
}

// =====================================================================
// §3 / issue #3115 — ACCEPT-CONTROL: ordinary primitive-to-primitive
// assignments remain admitted. The reject gate fires only on `Object`
// as source, so this baseline must keep its pre-existing diagnostic-free
// behavior.
// =====================================================================

BEZEL_TEST(sema_init_value_from_object_3115,
           primitive_literal_local_accepted_spec_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 42; "
        "    var j: int := i; "
        "    return j; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kIssue3115Diag));
    BEZEL_EXPECT_TRUE(!hasError(r, kParagraph7Nullable));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           primitive_widening_local_accepted_spec_5_1) {
    // int → long (widening per the §5.1 numeric-promotion table) is the
    // canonical primitive-primitive accept and must stay clean.
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 7; "
        "    var l: long := i; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kIssue3115Diag));
}

// =====================================================================
// §3 / issue #3115 — REJECT (REGRESSION): an Object-typed return from a
// method whose declared return is a value type is the same defect class —
// it is the return site of checkAssignable. Pre-fix the same silent-accept
// trap fired; the fix to the top of checkAssignable closes it at every
// caller of checkAssignable, not just LocalDecl. The brief's scoping
// "value-typed local" reads narrowly; the issue body's "or assigned to a
// value-typed location" reads broadly. Pin the broader reading in test
// form so a future narrowing regression to the LocalDecl-only fix is
// caught.
// =====================================================================

BEZEL_TEST(sema_init_value_from_object_3115,
           object_return_into_int_method_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    return o; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

// =====================================================================
// §3 / issue #3115 — REJECT: an Object initializer into a Named-kind
// target (user record / user enum / user choice / interface). The fix's
// predicate fires on `from Primitive(Object)` regardless of `to`'s kind
// (Primitive OR Named), so these cases must all reject — pre-fix they
// were silent-accept. Without these, a regression that narrows the gate
// to fire only on Primitive-kind `to` (the cheapest implementation
// choice) would pass the existing primitive-target suite.
// =====================================================================

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_user_record_local_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var p: Point := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_user_enum_local_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color: Red, Green, Blue; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var c: Color := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_user_choice_local_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "choice Shape: Circle(int), Square(int); "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var s: Shape := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_interface_target_local_rejected_spec_3_5_1) {
    // The `o` source carries a class type (`A`) that implements the
    // target interface `IFoo`. Pre-fix the unchecked downcast admitted
    // this and the first method call on the target saw instance
    // confusion; the fix rejects the assignment outright.
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo: M(): void; end interface; "
        "class A: implements IFoo; "
        "  public method M(): void => end method; "
        "end A; "
        "class C "
        "  public static method Main(): int "
        "    var a := new A; "
        "    var o: Object := a; "
        "    var f: IFoo := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

// =====================================================================
// §3 / issue #3115 — REJECT: every Primitive-kind target width must
// reject. The brief listed `int`, `bool`, `char`, `byte`, `long`,
// `decimal`, `string`. The existing suite covers `int` (init / assign
// / return), `string`, `DateTime`. The remainder are pinned here so a
// regression that narrows the gate to "only `int` Primitive target"
// (a tempting optimization) is caught.
// =====================================================================

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_bool_local_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var b: bool := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_char_local_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var ch: char := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_byte_local_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var by: byte := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_long_local_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var l: long := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

BEZEL_TEST(sema_init_value_from_object_3115,
           object_into_decimal_local_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var d: decimal := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}

// =====================================================================
// §3 / issue #3115 — REJECT: CALL-ARG position. The fix lives in
// `checkAssignable`, which is the assignment-context gate every
// initializer, assignment, return, AND call argument pass through.
// Pinning this case proves the gate fires at call sites — a regression
// to "fix only the LocalDecl path" (e.g. patching `lowerLocalDecl`
// instead of the upstream checkAssignable) would slip past the
// existing suite.
// =====================================================================

BEZEL_TEST(sema_init_value_from_object_3115,
           object_as_call_arg_to_int_param_rejected_spec_3_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Helper "
        "  public static method TakeInt(x: int): int => x; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var v := Helper.TakeInt(o); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kIssue3115Diag));
}
