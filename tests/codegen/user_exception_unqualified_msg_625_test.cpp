// Codegen tests for issue #625 — unqualified `Message` inside a user class
// that inherits a built-in exception is an "unresolved identifier" error.
//
// Root cause: implicit-`this` member lookup in `lookupName` walks
// TypeSymbol::baseTypes but built-in exception types have no TypeSymbol and
// therefore no members in that chain.  The #606 fix patched the qualified
// MemberAccessExpr path (`e.Message`); the unqualified path was missed.
//
// Fix (sema): when an identifier is unresolved and its name is `Message` and
// `currentType_` inherits a built-in exception (same AST-base walk as #606),
// suppress the "unresolved identifier" diagnostic and set the expr type to
// `string` — exactly as the qualified `this.Message` would.
//
// Fix (codegen): when lowering an IdentifierExpr with `name == "Message"`,
// no resolved symbol, and a live `currentThis_`, emit the GEP-at-byte-8 read
// (same as the qualified `this.Message` CG22+ path in LlvmBackend_Exprs.cpp).
//
// per §12 (inheritance), §31 (exceptions), stdlib Exception.Message.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool hasError(const std::string& src, const char* kw) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
                d.message.find(kw) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── Group 1: basic sema acceptance ───────────────────────────────────────────

// #625 / §12 / §31: unqualified `Message` in a method body of an exception-
// derived class must NOT produce "unresolved identifier".
BEZEL_TEST(codegen_625, sema_no_unresolved_in_override_spec_12_31) {
    BEZEL_EXPECT_EQ(
        hasError(
            "module M; "
            "class AppError: Exception "
            "  public constructor(msg: string) base(msg); end constructor; "
            "  public override method ToString(): string => \"[\" + Message + \"]\"; "
            "end AppError; "
            "class C public static method Main(): int return 0; end Main; end C;",
            "unresolved identifier"),
        false);
}

// #625 / §12 / §31: unqualified `Message` in a plain non-override method body
// must also be accepted.
BEZEL_TEST(codegen_625, sema_no_unresolved_in_plain_method_spec_12_31) {
    BEZEL_EXPECT_EQ(
        hasError(
            "module M; "
            "class AppError: Exception "
            "  public constructor(msg: string) base(msg); end constructor; "
            "  public method GetMsg(): string => Message; "
            "end AppError; "
            "class C public static method Main(): int return 0; end Main; end C;",
            "unresolved identifier"),
        false);
}

// ── Group 2: runtime correctness ─────────────────────────────────────────────

// #625 / §12 / §31: `ToString()` override uses bare `Message` — calling it
// must produce "[x]" whose length is 3.
BEZEL_TEST(codegen_625, tostring_override_with_bare_message_returns_correct_length_spec_12_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "  public override method ToString(): string => \"[\" + Message + \"]\"; "
        "end AppError; "
        "class C public static method Main(): int "
        "  try "
        "    throw new AppError(\"x\"); "
        "  catch var e: AppError => do "
        "    var s := e.ToString(); "
        "    return s.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        3);   // "[x]".Length = 3
}

// #625 / §12 / §31: plain method `GetMsg()` using bare `Message` returns the
// correct message content length.
BEZEL_TEST(codegen_625, plain_method_bare_message_correct_length_spec_12_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "  public method GetMsg(): string => Message; "
        "end AppError; "
        "class C public static method Main(): int "
        "  try "
        "    throw new AppError(\"hello\"); "
        "  catch var e: AppError => return e.GetMsg().Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        5);   // "hello".Length = 5
}

// #625 / §12 / §31: bare `Message` after `base(msg)` in ctor body is not
// tested here (ctors can't use bare `Message` as a value-expression in the
// same way) — focus is on method bodies.

// #625 / §12 / §31: two-level user hierarchy — `AppError: Exception`,
// `SpecificError: AppError` — unqualified `Message` in the leaf's method body
// must resolve through the full ancestor chain.
BEZEL_TEST(codegen_625, two_level_hierarchy_bare_message_spec_12_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end AppError; "
        "class SpecificError: AppError "
        "  public constructor(msg: string) base(msg); end constructor; "
        "  public override method ToString(): string => \"[\" + Message + \"]\"; "
        "end SpecificError; "
        "class C public static method Main(): int "
        "  try "
        "    throw new SpecificError(\"hi\"); "
        "  catch var se: SpecificError => return se.ToString().Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        4);   // "[hi]".Length = 4
}

// ── Group 3: regression guards ────────────────────────────────────────────────

// #625 / §31: catch-variable `.Message` (from #606/#607) must still work.
// Note: `this.Message` inside a user method body is a separate pre-existing
// issue unrelated to #625; it is not tested here.
BEZEL_TEST(codegen_625, catch_var_message_regression_spec_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end AppError; "
        "class C public static method Main(): int "
        "  try "
        "    throw new AppError(\"world\"); "
        "  catch var e: AppError => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        5);   // "world".Length = 5
}
