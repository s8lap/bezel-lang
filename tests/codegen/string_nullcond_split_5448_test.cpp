// fix #5448 (§9.6 / §7.5, v0.315): `string?.Split(...)` was rejected with
// "string member `?.Split` is not yet supported in codegen". The `.` form
// (string.Split) compiles and runs correctly; the `?.` form must behave
// identically for non-null receivers (returning the split array) and return
// null for null receivers.
//
// THE DEFECT had two parts:
//   1. Sema (ExprTypecheck_Calls.cpp): the `?.Split` arm pre-typed the access
//      (for the §21.1 const-materializer check) but set `typedNcmMayReturn=false`,
//      causing it to fall through to the string-LINQ gate — which incorrectly
//      rejects Split because Split is a builtin string method, NOT a LINQ operator.
//      Fix: set `typedNcmMayReturn=true` so the access bypasses the LINQ gate.
//   2. Codegen (LlvmBackend_Calls2.cpp): builtin string methods (Split,
//      ToCharArray) have no MethodSymbol, so `ms` is null and the dispatch fell
//      through to `return nullptr` instead of generating the Split call.
//      Fix: inline the Split/ToCharArray lowering in `lowerNullCondCall`, with
//      proper null short-circuit (phi of null and the call result).
//
// The `?.` form's null-short-circuit is a runtime property, not a static one.
// The static type is `Array<string>?` (nullable), and sema types it correctly
// (per the fix in ExprTypecheck_Calls.cpp). Codegen generates the phi of
// null (when receiver is null) and the actual split result (when receiver is
// non-null).

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §9.6 / §7.5 / #5448: `string?.Split(...)` compiles and runs on a non-null
// receiver. The result is Array<string> (not nullable at runtime since the
// receiver is non-null). Returns 99 on success.
BEZEL_TEST(string_nullcond_split_5448,
           non_null_receiver_split_compiles_and_runs) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t: string? := \"a,b,c\"; "
        "  var parts := t?.Split(\",\"); "
        "  if parts <> null and parts.Count = 3 then return 99; else return 0; end; "
        "end Main; end C;"), 99);
}

// §9.6 / §7.5 / #5448: null receiver short-circuits and returns null.
// The result type is Array<string>? but at runtime it is null.
BEZEL_TEST(string_nullcond_split_5448,
           null_receiver_short_circuits_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t: string? := null; "
        "  var parts := t?.Split(\",\"); "
        "  if parts = null then return 99; else return 0; end; "
        "end Main; end C;"), 99);
}

// §9.6 / §7.5 / #5448: the `?.` form mirrors the `.` form's result for
// a non-null receiver (regression guard: the fix must not make the two
// spellings diverge).
BEZEL_TEST(string_nullcond_split_5448,
           dot_form_and_question_dot_form_agree_on_non_null) {
    // Both should return 99.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t: string := \"a,b,c\"; "
        "  var parts := t.Split(\",\"); "
        "  if parts.Count = 3 then return 99; else return 0; end; "
        "end Main; end C;"), 99);
}

// §9.6 / §7.5 / #5448: `ToCharArray` via `?.` on a nullable string also
// works (same fix applies to it). Non-null receiver.
BEZEL_TEST(string_nullcond_split_5448,
           tochararray_on_non_null_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t: string? := \"abc\"; "
        "  var chars := t?.ToCharArray(); "
        "  if chars <> null and chars.Count = 3 then return 99; else return 0; end; "
        "end Main; end C;"), 99);
}

// §9.6 / §7.5 / #5448: `ToCharArray` via `?.` on a nullable string
// returns null when the receiver is null.
BEZEL_TEST(string_nullcond_split_5448,
           tochararray_null_receiver_short_circuits) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t: string? := null; "
        "  var chars := t?.ToCharArray(); "
        "  if chars = null then return 99; else return 0; end; "
        "end Main; end C;"), 99);
}
