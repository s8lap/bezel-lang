// Spec-derived tests for program entry-point resolution (§33.5), using the
// §33.2 accepted-signature set.
//
// §33.1 — "Every Bezel executable has exactly one entry point: a `public
//   static method Main` declared on any class in the program's main module."
// §33.2 — the eight accepted signatures:
//     Main()                              Main(args: Array<string>)
//     Main(): int                         Main(args: Array<string>): int
//     async Main(): Task                  async Main(args: Array<string>): Task
//     async Main(): Task<int>             async Main(args: Array<string>): Task<int>
//   "A method named `Main` with a non-qualifying signature is an ordinary
//   method." "`Main` must be `public` and `static`."
// §33.5 — "A program is ill-formed if more than one method qualifies as the
//   entry point" (across classes or within one class); the compiler errors
//   listing every qualifying `Main`. Zero qualifying entry points in an
//   executable build is likewise an error (§33.1).
//
// checkEntryPoint is the executable-build-only check the driver runs; these
// tests drive it directly after the shared front-end passes.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/EntryPoint.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

// Run the front-end passes the driver runs before the entry-point check, then
// invoke checkEntryPoint. Returns true iff exactly one qualifying entry point
// was found (no §33.5 diagnostic).
bool resolvesToSingleEntryPoint(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (!r.sourceFile || !r.ctx) return false;
    bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    return bezel::sema::checkEntryPoint(*r.sourceFile, *r.ctx);
}

// True iff some emitted diagnostic is the §33.5 / §33.1 entry-point message.
bool hasEntryPointError(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (!r.sourceFile || !r.ctx) return true;
    bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    bezel::sema::checkEntryPoint(*r.sourceFile, *r.ctx);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("entry point") != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exactly one qualifying Main — accepted (§33.5).
// ---------------------------------------------------------------------------

// §33.2: `Main(): int` is an accepted signature.
BEZEL_TEST(sema_entry_point, single_main_int_ok_33_5) {
    BEZEL_EXPECT_TRUE(resolvesToSingleEntryPoint(
        "module M; class C "
        "public static method Main(): int return 0; end Main; end C;"));
}

// §33.2: `Main()` (unit return) is an accepted signature.
BEZEL_TEST(sema_entry_point, single_main_unit_ok_33_5) {
    BEZEL_EXPECT_TRUE(resolvesToSingleEntryPoint(
        "module M; class C "
        "public static method Main() return; end Main; end C;"));
}

// §33.2: `Main(args: Array<string>): int` is an accepted signature.
BEZEL_TEST(sema_entry_point, single_main_args_int_ok_33_5) {
    BEZEL_EXPECT_TRUE(resolvesToSingleEntryPoint(
        "module M; class C "
        "public static method Main(args: Array<string>): int "
        "return 0; end Main; end C;"));
}

// §33.2: a non-qualifying `Main` (instance method, wrong signature) does not
// conflict — the single static qualifying `Main` resolves cleanly. Here a
// differently-named static helper plus one qualifying Main.
BEZEL_TEST(sema_entry_point, helper_plus_single_main_ok_33_2) {
    BEZEL_EXPECT_TRUE(resolvesToSingleEntryPoint(
        "module M; class C "
        "public static method Helper(): int return 1; end Helper; "
        "public static method Main(): int return 0; end Main; end C;"));
}

// ---------------------------------------------------------------------------
// Zero qualifying Main — rejected (§33.1).
// ---------------------------------------------------------------------------

// §33.1: an executable with no `Main` at all has no entry point.
BEZEL_TEST(sema_entry_point, zero_main_rejected_33_1) {
    BEZEL_EXPECT_TRUE(hasEntryPointError(
        "module M; class C "
        "public static method Helper(): int return 0; end Helper; end C;"));
}

// §33.2: a `Main` that is not `public` does not qualify → zero entry points.
BEZEL_TEST(sema_entry_point, private_main_does_not_qualify_33_2) {
    BEZEL_EXPECT_TRUE(hasEntryPointError(
        "module M; class C "
        "private static method Main(): int return 0; end Main; end C;"));
}

// §33.2: a `Main` with a non-qualifying signature (an arbitrary `int`
// parameter) is an ordinary method → zero entry points.
BEZEL_TEST(sema_entry_point, wrong_signature_main_does_not_qualify_33_2) {
    BEZEL_EXPECT_TRUE(hasEntryPointError(
        "module M; class C "
        "public static method Main(x: int): int return 0; end Main; end C;"));
}

// ---------------------------------------------------------------------------
// More than one qualifying Main — rejected (§33.5).
// ---------------------------------------------------------------------------

// §33.5 cross-class: two classes each declaring a qualifying `Main`.
BEZEL_TEST(sema_entry_point, two_mains_across_classes_rejected_33_5) {
    BEZEL_EXPECT_FALSE(resolvesToSingleEntryPoint(
        "module M; "
        "class A public static method Main(): int return 0; end Main; end A; "
        "class B public static method Main(): int return 1; end Main; end B;"));
    BEZEL_EXPECT_TRUE(hasEntryPointError(
        "module M; "
        "class A public static method Main(): int return 0; end Main; end A; "
        "class B public static method Main(): int return 1; end Main; end B;"));
}
