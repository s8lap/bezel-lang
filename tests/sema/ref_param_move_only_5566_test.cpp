// fix #5566 / spec §22.3 *Parameter modifiers* (v0.338), `ref` bullet: "A `ref`
// parameter whose declared type is a move-only value type (§15.3; an owning
// choice's nullable wrapper `C?` among them, §35.9), a function type's `ref`
// slot included, is a compile error at the declaration: no argument can bind
// it, since every location of that type is owned state at a `ref` argument."
// A `ref` parameter whose declared type is or contains a type parameter "is not
// rejected at its declaration".
//
// Before the fix no declaration-side check existed: `ref c: Bundle` over an
// owning choice was admitted silently (src/sema had no `ref`-parameter site for
// common::isMoveOnlyValueType). The reject rows assert the §22.3 needle and the
// `(§22.3)` citation, never `hasErrors()` alone. The control rows assert
// `r.ran` and the program's stdout, so a skipped program cannot pass.

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasRefDiagnostic(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos &&
            d.message.find("(§22.3)") != std::string::npos)
            return true;
    return false;
}

// Shared declarations: a class to own, an owning choice (an owned class payload
// classifies the choice Owning, §35.4), and a copyable choice and record for
// the controls.
const std::string kPrelude =
    "module M; "
    "class Res public constructor() end constructor; end Res; "
    "choice Bundle Ready(owned first: Res), Empty; end Bundle; "
    "choice Color Red, Green; end Color; "
    "record Pt(X: int, Y: int); ";

}  // namespace

// --- reject rows: a `ref` parameter at a move-only value type ----------------

BEZEL_TEST(sema_ref_param_move_only_5566,
           method_ref_owning_choice_rejected_sec_22_3) {
    // §22.3 `ref` bullet, the issue's first repro: `method F(ref c: C)` over an
    // owning choice.
    auto r = runAot(kPrelude +
                    "class C public static method F(ref c: Bundle) end F; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnostic(
        r, "a `ref` parameter `c` of `F` may not be declared at an owning-classified choice"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           method_ref_nullable_owning_choice_rejected_sec_22_3) {
    // §22.3 `ref` bullet: "an owning choice's nullable wrapper `C?` among them"
    // (§35.9). The issue's second repro, `ref c: C?`.
    auto r = runAot(kPrelude +
                    "class C public static method F(ref c: Bundle?) end F; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnostic(
        r, "a `ref` parameter `c` of `F` may not be declared at an owning-classified choice"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           method_ref_owning_inline_array_rejected_sec_22_3) {
    // §22.3 `ref` bullet: "an owning inline array" is a move-only value type
    // (§15.3, kinds in §15.3's second paragraph).
    auto r = runAot(kPrelude +
                    "class C public static method F(ref a: [2 owned]Res) end F; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnostic(
        r, "a `ref` parameter `a` of `F` may not be declared at an owning inline array"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           constructor_ref_owning_choice_rejected_sec_22_3) {
    // §22.3 `ref` bullet: "(method, constructor, lambda, interface member,
    // function type)" — the constructor site. The diagnostic names the class.
    auto r = runAot(kPrelude +
                    "class C public constructor(ref c: Bundle) end constructor; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnostic(
        r, "a `ref` parameter `c` of `C` may not be declared at an owning-classified choice"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           function_type_ref_slot_owning_choice_rejected_sec_22_3) {
    // §22.3 `ref` bullet: "a function type's `ref` slot included". The slot sits
    // in a method's parameter type `(ref Bundle) -> ()`.
    auto r = runAot(kPrelude +
                    "class C public static method H(f: (ref Bundle) -> ()) end H; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnostic(
        r, "a function type's `ref` slot may not be declared at an owning-classified choice"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           interface_method_ref_owning_choice_rejected_sec_22_3) {
    // §22.3 `ref` bullet: the interface-member site.
    auto r = runAot(kPrelude +
                    "interface IBox method M(ref c: Bundle); end IBox; "
                    "class C public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnostic(
        r, "a `ref` parameter `c` of `M` may not be declared at an owning-classified choice"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           extension_method_ref_owning_choice_rejected_sec_22_3) {
    // §22.3 `ref` bullet: the extension-member site (§26), an instance method
    // of an extension block over a record.
    auto r = runAot(kPrelude +
                    "public extension Pt public method M(ref c: Bundle) end M; end extension; "
                    "class C public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnostic(
        r, "a `ref` parameter `c` of `M` may not be declared at an owning-classified choice"));
}

// --- control rows: must still ACCEPT and RUN ----------------------------------

BEZEL_TEST(sema_ref_param_move_only_5566,
           plain_out_owned_params_at_owning_choice_still_accepted_sec_22_3) {
    // §22.3: the rejection is for `ref` only. A plain by-value parameter (§15.3
    // *Parameter passing*), an `owned` one and an `out` one stay legal at the
    // same type. The `out` body assigns its parameter on the return path.
    auto r = runAot(kPrelude +
                    "class C "
                    "public static method P(c: Bundle) end P; "
                    "public static method O(owned c: Bundle) end O; "
                    "public static method U(out c: Bundle) c := Bundle.Empty; end U; "
                    "public static method Main() Console.WriteLine(\"ran\"); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           ref_at_copyable_value_types_still_accepted_sec_22_3) {
    // §22.3 `ref` bullet: only a move-only declared type is rejected. A copyable
    // choice, a record and an `int` keep their `ref` parameters.
    auto r = runAot(kPrelude +
                    "class C "
                    "public static method A(ref c: Color) end A; "
                    "public static method B(ref p: Pt) end B; "
                    "public static method N(ref n: int) end N; "
                    "public static method Main() Console.WriteLine(\"ran\"); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           ref_at_generic_type_parameter_declaration_accepted_sec_22_3) {
    // §22.3 `ref` bullet: "A `ref` parameter whose declared type is or contains
    // a type parameter is not rejected at its declaration." Declaration only;
    // the program never instantiates the method at a move-only type.
    auto r = runAot(kPrelude +
                    "class C public static method G<T>(ref x: T) end G; "
                    "public static method Main() Console.WriteLine(\"ran\"); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}

// --- nested and alias rows (fix #5566 recursion, §11.6 v0.286 nested types) ---

namespace {

// As hasRefDiagnostic, and additionally requires the diagnostic to carry a
// `file:line:col: ` position immediately before the needle. runAot's diagnostic
// is the captured compiler stderr (its SourceLocation is not transported), so
// the position is read from the text the driver printed.
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

}  // namespace

BEZEL_TEST(sema_ref_param_move_only_5566,
           nested_class_method_ref_owning_choice_rejected_sec_22_3) {
    // §22.3 `ref` bullet with §11.6 nested types: the same method as
    // `method_ref_owning_choice_rejected_sec_22_3`, but declared inside a
    // class nested in a class. The check must reach it.
    auto r = runAot(kPrelude +
                    "class E4 public class In public static method F(ref b: Bundle) end F; "
                    "end In; end E4; "
                    "class C public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(
        r, "a `ref` parameter `b` of `F` may not be declared at an owning-classified choice"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           doubly_nested_constructor_ref_owning_choice_rejected_sec_22_3) {
    // §22.3 `ref` bullet, constructor site, two levels of §11.6 nesting:
    // class C > class Mid > class Deep, with the constructor on Deep.
    auto r = runAot(kPrelude +
                    "class C public class Mid public class Deep "
                    "public constructor(ref b: Bundle) end constructor; "
                    "end Deep; end Mid; end C; "
                    "class D public static method Main() end Main; end D;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(
        r, "a `ref` parameter `b` of `Deep` may not be declared at an owning-classified choice"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           alias_of_nullable_owning_choice_ref_rejected_sec_22_3) {
    // §22.3 `ref` bullet: the rule looks at the declared type's classification,
    // so a type alias to the nullable owning choice (`Bundle?`, §35.9) is
    // rejected at the `ref` parameter exactly as the spelled-out type is.
    auto r = runAot(kPrelude +
                    "type BQ := Bundle?; "
                    "class C public static method F(ref b: BQ) end F; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(
        r, "a `ref` parameter `b` of `F` may not be declared at an owning-classified choice"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           ref_owning_inline_array_nullable_rejected_sec_22_3) {
    // §22.3 `ref` bullet: "an owning inline array" is move-only (§15.3). The
    // nullable wrapper of an owning inline array, `[2 owned]Res?`, is also
    // move-only; a `ref` parameter of that type is rejected at its declaration.
    auto r = runAot(kPrelude +
                    "class C public static method F(ref a: [2 owned]Res?) end F; "
                    "public static method Main() end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasRefDiagnosticWithLocation(
        r, "a `ref` parameter `a` of `F` may not be declared at an owning inline array"));
}

BEZEL_TEST(sema_ref_param_move_only_5566,
           nested_class_method_ref_copyable_choice_still_accepted_sec_22_3) {
    // §22.3 `ref` bullet, control: a copyable choice at a `ref` parameter of a
    // nested class's method is not move-only and keeps its `ref` parameter.
    auto r = runAot(kPrelude +
                    "class E4 public class In public static method F(ref b: Color) "
                    "Console.WriteLine(\"ran\"); end F; end In; end E4; "
                    "class C public static method Main() var c := Color.Red; "
                    "E4.In.F(ref c); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}
