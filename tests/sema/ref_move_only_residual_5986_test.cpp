// fix #5986 (residual of #5566) / spec §22.3 *Parameter modifiers* (v0.338),
// `ref` bullet: "A `ref` parameter whose declared type is a move-only value
// type (§15.3; an owning choice's nullable wrapper `C?` among them, §35.9), a
// function type's `ref` slot included, is a compile error at the declaration:
// no argument can bind it, since every location of that type is owned state
// at a `ref` argument."
//
// #5566 covered the parameter positions (methods, constructors, interface
// members) and a function type that is itself a parameter type. This file
// covers the residual lambda position (a): a lambda's written `ref` parameter.
// The function-type `ref` slot at the other written positions (b) is in the
// next commit of this lane.
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

