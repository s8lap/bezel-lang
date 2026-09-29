// Spec-derived regression test for issue #5023.
//
// §3.1 / §3.2 (v0.307), *Type kinds* / *Reference vs. value types*:
//   "A **value type** is copied on assignment; each variable holds an
//    independent value. Records, enums, choices (§35), primitives, inline
//    arrays (`[N]T`, including `[N]T?` and `[N owned]T?`/`[N owned]T`), and
//    the unit type are value types."
//   "Choices — user-defined value types whose values carry one of a fixed
//    set of named cases, each with optional payloads."
//
// §5.1 (v0.307), *Implicit conversions* / *Reference conversions*: the
// admitted conversions TO `Object` list "value types (primitives, records,
// enums, non-owning inline arrays, the unit type) by boxing" — there is no
// entry anywhere in §5.1 for the REVERSE direction, `Object` to a value
// type. No implicit `Object -> value type` conversion exists for any
// member of the §3.1 value-type population.
//
// §17.4, *Degenerate value-type targets* / *is/is not/as*: the sanctioned
// way to read a value-typed payload out of an `Object` is the §12.7
// flow-narrowing form (`if o is T then var v: T := o; end;`) or the
// `as` cast, yielding `T?`.
//
// issue #5023: `src/sema/ExprTypecheck.cpp`'s `checkAssignable` already
// rejected the scalar case (`var k: int := o;`, issue #3115) but its
// value-typed-target predicate covered only `TypeRef::Kind::Primitive`
// (non-Object, non-string) and the Named value-BUILTINS (`DateTime` &
// friends via `isValueBuiltinByName`). It did not cover
// `TypeRef::Kind::InlineArray` (`[N]T`) or `TypeRef::Kind::Named` types
// declared as `TypeDeclKind::Record` / `::Enum` / `::Choice` — so
// `var b: [3]int := o;`, `var p: Pt := o;` (a record), `var d: Color := o;`
// (an enum), and `var oc: Outcome := o;` (a choice) were all silently
// ACCEPTED for an unnarrowed `o: Object`, reproducing exactly the
// box-pointer-reinterpreted-as-value defect #3115 closed for scalars
// (codegen's `lowerLocalDecl` stores the `ptr`-typed slot at the value's
// storage and a later load reinterprets the box pointer as the value).
//
// This file pins the extension: REJECT for inline-array, record, enum,
// and choice targets (init / assignment / return / call-arg positions,
// mirroring the position coverage `init_value_from_object_3115_test.cpp`
// already pins for the scalar case), plus ACCEPT-CONTROLS for the
// legitimate paths that must stay admitted — the §12.7 narrowed copy-out,
// `T -> Object` boxing, and ordinary value-to-value-typed-target copies.

#include <initializer_list>
#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §5.1 diagnostic from checkAssignable (shared with issue #3115's
// scalar case — the extension reuses the same message, naming the same
// two remedies).
constexpr const char* kValueTypeDiag =
    "has type `Object` which has no implicit conversion";

}  // namespace

// =====================================================================
// §3.1/§3.2/§5.1, issue #5023 — REJECT: inline-array target `[3]int`,
// unnarrowed `Object` source. This is the issue's own repro shape.
// =====================================================================

BEZEL_TEST(sema_object_to_value_type_target_5023,
           object_into_inline_array_local_rejected_spec_3_1_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var arr: [3]int := [1, 2, 3]; "
        "    var o: Object := arr; "
        "    var b: [3]int := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kValueTypeDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "§12.7"));
    BEZEL_EXPECT_TRUE(hasError(r, "§17.4"));
}

BEZEL_TEST(sema_object_to_value_type_target_5023,
           object_assignment_into_inline_array_local_rejected_spec_3_1_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var arr: [3]int := [1, 2, 3]; "
        "    var o: Object := arr; "
        "    var b: [3]int := [0, 0, 0]; "
        "    b := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kValueTypeDiag));
}

BEZEL_TEST(sema_object_to_value_type_target_5023,
           object_return_into_inline_array_method_rejected_spec_3_1_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Get(): [3]int "
        "    var arr: [3]int := [1, 2, 3]; "
        "    var o: Object := arr; "
        "    return o; "
        "  end Get; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kValueTypeDiag));
}

BEZEL_TEST(sema_object_to_value_type_target_5023,
           object_as_call_arg_to_inline_array_param_rejected_spec_3_1_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Helper "
        "  public static method TakeArr(x: [3]int): int => x[0]; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var arr: [3]int := [1, 2, 3]; "
        "    var o: Object := arr; "
        "    var v := Helper.TakeArr(o); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kValueTypeDiag));
}

// =====================================================================
// §3.1/§5.1, issue #5023 — REJECT: user-declared record target, unnarrowed
// `Object` source. §3.1 lists records among the value-type population;
// records are NOT covered by the "reference-typed Named" carve-out that
// keeps `Object -> interface` / `Object -> string` permissive.
// =====================================================================

BEZEL_TEST(sema_object_to_value_type_target_5023,
           object_into_record_local_rejected_spec_3_1_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(7, 9); "
        "    var o: Object := p; "
        "    var q: Pt := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kValueTypeDiag));
}

// =====================================================================
// §3.1/§5.1, issue #5023 — REJECT: user-declared enum target, unnarrowed
// `Object` source.
// =====================================================================

BEZEL_TEST(sema_object_to_value_type_target_5023,
           object_into_enum_local_rejected_spec_3_1_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color "
        "  Red, Green, Blue; "
        "end Color; "
        "class C "
        "  public static method Main(): int "
        "    var c: Color := Color.Red; "
        "    var o: Object := c; "
        "    var d: Color := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kValueTypeDiag));
}

// =====================================================================
// §3.1/§5.1/§35, issue #5023 — REJECT: user-declared choice target,
// unnarrowed `Object` source. §3.1: "Choices — user-defined value types
// whose values carry one of a fixed set of named cases" — a choice is a
// value type on EVERY classification (value-like, borrow-bearing, owning;
// §35.4), so no classification check gates this rejection.
// =====================================================================

BEZEL_TEST(sema_object_to_value_type_target_5023,
           object_into_choice_local_rejected_spec_3_1_5_1_35) {
    auto r = parseSemaAll(
        "module M; "
        "choice Outcome "
        "  Ok(value: int), "
        "  Failed; "
        "end Outcome; "
        "class C "
        "  public static method Main(): int "
        "    var oc: Outcome := Outcome.Ok(3); "
        "    var o: Object := oc; "
        "    var d: Outcome := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kValueTypeDiag));
}

// =====================================================================
// §12.7/§17.4, issue #5023 — ACCEPT-CONTROL: the sanctioned §12.7
// narrowed copy-out for an inline-array target must stay admitted. The
// narrowed identifier's exprType is `[3]int` (not `Object`) inside the
// controlled block (fix #3090), so `checkAssignable`'s `from` never
// reaches the `Primitive(Object)` branch this fix extends.
// =====================================================================

BEZEL_TEST(sema_object_to_value_type_target_5023,
           narrowed_inline_array_copy_out_accepted_spec_12_7) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var arr: [3]int := [1, 2, 3]; "
        "    var o: Object := arr; "
        "    if o is [3]int then "
        "      var b: [3]int := o; "
        "      return b[0]; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kValueTypeDiag));
}

BEZEL_TEST(sema_object_to_value_type_target_5023,
           narrowed_record_copy_out_accepted_spec_12_7) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(7, 9); "
        "    var o: Object := p; "
        "    if o is Pt then "
        "      var q: Pt := o; "
        "      return q.X; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kValueTypeDiag));
}

// =====================================================================
// §5.1/§8.6, issue #5023 — ACCEPT-CONTROL: `T -> Object` boxing (the
// forward direction, unaffected by this fix) stays admitted for an
// inline array.
// =====================================================================

BEZEL_TEST(sema_object_to_value_type_target_5023,
           inline_array_into_object_local_accepted_boxing) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var arr: [3]int := [1, 2, 3]; "
        "    var o: Object := arr; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kValueTypeDiag));
}

// =====================================================================
// §3.2, issue #5023 — ACCEPT-CONTROL: an ordinary inline-array-to-
// inline-array value copy (no `Object` anywhere) stays admitted — the
// reject gate fires only when the SOURCE is `Object`.
// =====================================================================

BEZEL_TEST(sema_object_to_value_type_target_5023,
           inline_array_to_inline_array_copy_accepted_spec_3_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var a: [3]int := [1, 2, 3]; "
        "    var b: [3]int := a; "
        "    return b[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kValueTypeDiag));
}

// =====================================================================
// §5.1/§11, issue #5023 — ACCEPT-CONTROL (OUT OF SCOPE, unchanged):
// `Object -> interface` stays on the permissive path — interfaces are
// REFERENCE types (§3.2), not part of the §3.1 value-type population
// this fix closes, and iface dispatch handles the runtime check via the
// vtable. Pinning ACCEPT here guards against an over-broad future
// widening of this fix silently also catching interfaces.
// =====================================================================

BEZEL_TEST(sema_object_to_value_type_target_5023,
           object_into_interface_target_stays_accepted_out_of_scope) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo "
        "  method M(); "
        "end IFoo; "
        "class A : IFoo "
        "  public method M() "
        "  end M; "
        "end A; "
        "class C "
        "  public static method Main(): int "
        "    var a := new A(); "
        "    var o: Object := a; "
        "    var f: IFoo := o; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kValueTypeDiag));
}
