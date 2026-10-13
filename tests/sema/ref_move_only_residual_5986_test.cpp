// fix #5986 (residual of #5566) / spec §22.3 *Parameter modifiers* (v0.338),
// `ref` bullet: "A `ref` parameter whose declared type is a move-only value
// type (§15.3; an owning choice's nullable wrapper `C?` among them, §35.9), a
// function type's `ref` slot included, is a compile error at the declaration:
// no argument can bind it, since every location of that type is owned state
// at a `ref` argument."
//
// #5566 covered the parameter positions (methods, constructors, interface
// members) and a function type that is itself a parameter type. This file
// covers the two residual positions: (a) a lambda's written `ref` parameter,
// and (b) a function type's `ref` slot at every other position where the
// function type is written: a local, a field, a property, a method return
// type, a generic argument and an alias.
//
// The reject rows assert the §22.3 needle, the `(§22.3)` citation and the
// `file:line:col: ` position, never `hasErrors()` alone. The control rows
// assert `r.ran` and the program's stdout, so a skipped program cannot pass.

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// As the #5566 file's helper: the diagnostic must carry the needle, the
// `(§22.3)` citation and a `file:line:col: ` position before the needle.
bool hasRefDiagnosticWithLocation(const JitResult& r, const std::string& needle) {
    static const std::regex kPosition(":[0-9]+:[0-9]+: $");
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("(§22.3)") == std::string::npos) continue;
        const std::size_t at = d.message.find(needle);
        if (at == std::string::npos) continue;
        if (std::regex_search(d.message.substr(0, at), kPosition)) return true;
    }
    return false;
}

// Shared declarations: a class (not move-only), an owning choice (an owned
// class payload classifies the choice Owning, §35.4), and a copyable choice.
const std::string kPrelude =
    "module M; "
    "class Res public constructor() end constructor; end Res; "
    "choice Bundle Ready(owned first: Res), Empty; end Bundle; "
    "choice Color Red, Green; end Color; ";

const char* const kFnSlotNeedle =
    "a function type's `ref` slot may not be declared at an owning-classified choice";
const char* const kLambdaNeedle =
    "a `ref` parameter `b` of `lambda` may not be declared at an owning-classified choice";

}  // namespace

// --- (a) lambda `ref` parameter at a move-only value type -------------------

BEZEL_TEST(sema_ref_move_only_residual_5986,
           lambda_ref_owning_choice_untargeted_rejected_sec_22_3) {
    // §22.3 `ref` bullet, the lambda site: a `ref` parameter of a lambda with
    // no target type (the function type is inferred from the lambda itself).
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var f := lambda(ref b: Bundle) => do end; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kLambdaNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           lambda_ref_owning_choice_typed_local_rejected_sec_22_3) {
    // §22.3 `ref` bullet, the issue's repro (a): `var f: (ref Bundle) -> () :=
    // lambda(ref b: Bundle) => do end;`. The written local type is itself a
    // function type with a move-only `ref` slot, so the rule rejects the
    // declaration at both the local type and the lambda parameter.
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var f: (ref Bundle) -> () := lambda(ref b: Bundle) => do end; "
                    "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kLambdaNeedle));
}

// --- (b) function type's `ref` slot at a position other than a parameter -----

BEZEL_TEST(sema_ref_move_only_residual_5986,
           local_function_type_ref_slot_nullable_rejected_sec_22_3) {
    // §22.3 `ref` bullet: "a function type's `ref` slot included", as a LOCAL's
    // declared type (a nullable function type, §27.2).
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var f: ((ref Bundle) -> ())? := null; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           field_function_type_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a FIELD's declared function type.
    auto r = runAot(kPrelude +
                    "class C private _f: ((ref Bundle) -> ())? := null; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           property_function_type_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a PROPERTY's declared function type (block property,
    // §23.3), backed by a field of the same type.
    auto r = runAot(kPrelude +
                    "class C private _f: ((ref Bundle) -> ())? := null; "
                    "public property F: ((ref Bundle) -> ())? "
                    "get => _f; end F; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           method_return_function_type_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a method RETURN type that is a function type with a
    // move-only `ref` slot.
    auto r = runAot(kPrelude +
                    "class C public static method G(): ((ref Bundle) -> ())? "
                    "return null; end G; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           generic_argument_function_type_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a function type as a generic ARGUMENT (`Array<F>`).
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var a: Array<(ref Bundle) -> ()> := []; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           alias_function_type_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a function type written through a type alias
    // (`type FB := (ref Bundle) -> ();`), at the alias declaration.
    auto r = runAot(kPrelude +
                    "type FB := (ref Bundle) -> (); "
                    "class C public static method Main() var f: FB? := null; "
                    "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           local_function_type_ref_slot_owning_inline_array_rejected_sec_22_3) {
    // §22.3 `ref` bullet: "an owning inline array" is a move-only value type
    // (§15.3), so a function type's `ref` slot of that type is rejected too.
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var f: ((ref [2 owned]Res) -> ())? := null; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(
        r, "a function type's `ref` slot may not be declared at an owning inline array"));
}

// --- controls: must still accept (run) ---------------------------------------

BEZEL_TEST(sema_ref_move_only_residual_5986,
           lambda_ref_int_and_copyable_choice_still_accepted_sec_22_3) {
    // §22.3 `ref` bullet, control: a lambda `ref` parameter of a copyable type
    // (`int`, and the copyable choice `Color`) is not move-only and runs.
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var g: (ref int) -> () := lambda(ref n: int) => do end; "
                    "var k: int := 1; g(ref k); "
                    "var h: (ref Color) -> () := lambda(ref c: Color) => do end; "
                    "var c := Color.Red; h(ref c); "
                    "Console.WriteLine(\"ran\"); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           function_type_ref_slot_class_and_int_still_accepted_sec_22_3) {
    // §22.3 `ref` bullet, control: a function type's `ref` slot at an OWNING
    // CLASS (`Res`, a reference type, not move-only) and at `int` is accepted
    // in a local and a field.
    auto r = runAot(kPrelude +
                    "class C private _f: (ref Res) -> () := lambda(ref r: Res) => do end; "
                    "public static method Main() "
                    "var g: (ref int) -> () := lambda(ref n: int) => do end; "
                    "var o: (ref Res) -> () := lambda(ref r: Res) => do end; "
                    "Console.WriteLine(\"ran\"); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           function_type_ref_slot_type_parameter_still_accepted_sec_22_3) {
    // §22.3 `ref` bullet: "a `ref` parameter whose declared type is or contains
    // a type parameter is not rejected at its declaration". A function type
    // `(ref T) -> ()` at a type parameter is accepted as written.
    auto r = runAot(kPrelude +
                    "class Box<T> private _f: ((ref T) -> ())? := null; end Box; "
                    "class C public static method M<T>(f: (ref T) -> ()) end M; "
                    "public static method Main() Console.WriteLine(\"ran\"); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           function_type_plain_owned_out_slots_at_bundle_still_accepted_sec_22_3) {
    // §22.3 `ref` bullet, control: plain, `owned` and `out` slots of a
    // function type at the owning choice are not `ref` slots and stay legal,
    // at a local and a field.
    auto r = runAot(kPrelude +
                    "class C private _f: ((Bundle) -> ())? := null; "
                    "public static method Main() "
                    "var p: (Bundle) -> () := lambda(b: Bundle) => do end; "
                    "var o: (owned Bundle) -> () := lambda(owned b: Bundle) => do end; "
                    "var u: (out Bundle) -> () := lambda(out b: Bundle) => do b := Bundle.Empty; end; "
                    "Console.WriteLine(\"ran\"); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}

// --- follow-up (#5986 review): further written positions ---------------------

BEZEL_TEST(sema_ref_move_only_residual_5986,
           indexer_type_function_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a function type written as an INDEXER's value type
    // (`property [i: int]`, §23.4) carries the same `ref` slot.
    auto r = runAot(kPrelude +
                    "class C private _f: ((ref Bundle) -> ())? := null; "
                    "public property [i: int]: ((ref Bundle) -> ())? get => _f; end; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           interface_member_return_function_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a function type written as an INTERFACE member's
    // return type.
    auto r = runAot(kPrelude +
                    "interface IF method G(): ((ref Bundle) -> ())?; end IF; "
                    "class C public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           record_component_function_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a function type written as a RECORD component.
    auto r = runAot(kPrelude +
                    "record R(F: ((ref Bundle) -> ())?); "
                    "class C public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           choice_payload_function_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a function type written as a CHOICE case payload.
    auto r = runAot(kPrelude +
                    "choice H Hold(f: (ref Bundle) -> ()), Empty; end H; "
                    "class C public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           task_generic_argument_function_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a function type as the generic argument of `Task<…>`.
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var t: Task<(ref Bundle) -> ()>? := null; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           expression_new_array_function_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: an EXPRESSION-level function type, the type argument
    // of `new Array<…>(3)`, is written at the same position rule.
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var a := new Array<(ref Bundle) -> ()>(3); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           is_test_function_ref_slot_rejected_sec_22_3) {
    // §22.3 `ref` bullet: a function type written in an `is` test (an
    // expression-level type position) is rejected like any other written type.
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var o: Object := new Res(); "
                    "if o is ((ref Bundle) -> ())? then end; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           function_type_naming_choice_declared_later_rejected_sec_22_3) {
    // §22.3 `ref` bullet, classification-order hazard: the function type names
    // an owning choice declared AFTER its use. The decision waits for every
    // choice's classification, so the later declaration still decides it.
    auto r = runAot("module M; "
                    "class C public static method Main() "
                    "var f: ((ref Bundle) -> ())? := null; end Main; end C; "
                    "choice Bundle Ready(owned first: Res), Empty; end Bundle; "
                    "class Res public constructor() end constructor; end Res;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(r, kFnSlotNeedle));
}

// --- controls: a nullable copyable choice and a nullable owning class run ----

BEZEL_TEST(sema_ref_move_only_residual_5986,
           function_type_ref_nullable_copyable_choice_still_accepted_sec_22_3) {
    // §22.3 `ref` bullet, control: `Color?` is a nullable COPYABLE choice, not a
    // move-only value type (§35.9 applies only to an owning `C`); the slot runs.
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var f: ((ref Color?) -> ())? := null; "
                    "Console.WriteLine(\"ran\"); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}

BEZEL_TEST(sema_ref_move_only_residual_5986,
           function_type_ref_nullable_owning_class_still_accepted_sec_22_3) {
    // §22.3 `ref` bullet, control: `Res?` is the nullable of an owning CLASS,
    // a reference type and not a move-only value type; the slot runs.
    auto r = runAot(kPrelude +
                    "class C public static method Main() "
                    "var f: ((ref Res?) -> ())? := null; "
                    "Console.WriteLine(\"ran\"); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}
