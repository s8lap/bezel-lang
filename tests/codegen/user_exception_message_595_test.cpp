// Codegen tests for issue #595 — user-defined exception class inheriting from
// a built-in base (e.g. `class ErrA: Exception`) does not inherit the base's
// instance storage layout (the message slot), so `e.Message` reads garbage.
//
// Root cause: collectInstanceStorage only walks the TypeSymbol base chain,
// but built-in exception types have no TypeSymbol (declaration == nullptr) and
// are absent from TypeSymbol::baseTypes.  Fix: classStruct inserts a synthetic
// message-ptr slot at byte offset 8 (struct slot 1) for any user class whose
// AST-declared base chain includes a built-in exception.
//
// Tests organised into five groups:
//
//   1. Direct builtin-exception base (ErrA: Exception) — basic Message roundtrip.
//   2. Direct builtin-exception base — catch as the built-in Exception base.
//   3. User class adds own field PLUS inherits Message — both read correctly.
//   4. Two-level user hierarchy (ErrB: ErrA: Exception) — Message propagates.
//   5. Regression: existing built-in exception Message tests still pass.
//
// per §12 (inheritance), §31 (exceptions), stdlib Exception.Message.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Returns true iff the run produced an error diagnostic containing `keyword`.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runJit(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// ── Group 1: Direct builtin-exception base — basic Message roundtrip ──────────

// §12 / §31: `class ErrA: Exception` with `base(m)` constructor — the message
// passed to `base(m)` must be readable via `e.Message` after catching.
// Before fix: e.Message read garbage / wrong slot → wrong length or crash.
// After fix: message stored at byte offset 8, read correctly.
// per §31 / stdlib Exception.Message.
BEZEL_TEST(codegen_595, user_exception_base_message_basic_spec_12_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"hello\"); "
        "  catch var e: ErrA => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        5);   // "hello".Length = 5
}

// No-arg user exception constructor: base() not called, Message is empty string.
// per §31 / stdlib Exception.Message (null → empty string sentinel).
BEZEL_TEST(codegen_595, user_exception_no_base_call_message_empty_spec_12_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor() end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(); "
        "  catch var e: ErrA => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        0);   // message is null → empty string → Length 0
}

// User exception with a longer message string — check full content via length.
// per §12 / §31.
BEZEL_TEST(codegen_595, user_exception_message_longer_string_spec_12_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end AppError; "
        "class C public static method Main(): int "
        "  try "
        "    throw new AppError(\"bad value\"); "
        "  catch var e: AppError => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        9);   // "bad value".Length = 9
}

// ── Group 2: Catch as the same user type — arm executes ───────────────────────

// §12 / §31: throw user exception, catch as the same user type.
// This is the core runtime correctness test.
// per §31.2 (first matching arm).
BEZEL_TEST(codegen_595, user_exception_catch_arm_executes_spec_31_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class ErrB: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  var r := 0; "
        "  try "
        "    throw new ErrA(\"x\"); "
        "  catch var e: ErrA => r := e.Message.Length; end; "
        "  return r; "
        "end Main; end C;"),
        1);   // "x".Length = 1
}

// Two user exceptions — first matching arm wins.
// per §31.2.3.
BEZEL_TEST(codegen_595, user_exception_first_match_wins_spec_31_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class ErrB: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrB(\"bb\"); "
        "  catch var e: ErrA => return 1; "
        "  catch var e: ErrB => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        2);   // "bb".Length = 2
}

// ── Group 3: User class adds own field PLUS inherits Message ──────────────────

// §12: user exception adds its own `code: int` field.  Both `e.Message` (from
// the built-in Exception slot) and `e.code` (the user's own field) must read
// the correct values simultaneously.
// per §12 (prefix layout preserved: own fields come after inherited slots).
BEZEL_TEST(codegen_595, user_exception_own_field_plus_message_spec_12) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class ErrA: Exception "
        "  public _code: int; "
        "  public constructor(m: string, code: int) "
        "    base(m); "
        "    _code := code; "
        "  end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"oops\", 77); "
        "  catch var e: ErrA => do "
        "    Console.WriteLine(e.Message.Length * 100 + e._code); return 0; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        "477");   // 4 * 100 + 77 = 477
}

// ── Group 4: Two-level user hierarchy (ErrB: ErrA: Exception) ─────────────────

// §12 / §31: multi-level user exception hierarchy.  ErrB: ErrA: Exception.
// ErrB passes its message up through ErrA's ctor (base(m)) which calls
// base(m) again for Exception.  `e.Message` on `e: ErrB` must work.
// per §12 (prefix layout preserved across multiple user levels).
BEZEL_TEST(codegen_595, two_level_user_hierarchy_message_spec_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class ErrB: ErrA "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrB(\"deep\"); "
        "  catch var e: ErrB => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        4);   // "deep".Length = 4
}

// Two-level hierarchy: catch ErrB as ErrA (user-class hierarchy matching).
// ErrB: ErrA is a user-class inheritance, so the descriptor chain includes
// both td$ErrB and td$ErrA — `catch ErrA` fires.
// per §12 (ErrB is a subtype of ErrA).
BEZEL_TEST(codegen_595, two_level_hierarchy_caught_as_user_base_spec_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class ErrB: ErrA "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrB(\"chain\"); "
        "  catch var e: ErrA => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        5);   // "chain".Length = 5
}

// ── Group 5: Regression — sema rejects no-member error for Message on user ex ─

// §12 / §31: sema must NOT emit "no member `Message`" for a user exception
// class that inherits from Exception.  Before the fix this caused a sema error.
// per §31 / stdlib Exception.Message.
BEZEL_TEST(codegen_595, sema_no_member_error_for_message_spec_12_31) {
    BEZEL_EXPECT_EQ(
        hasError(
            "module M; "
            "class ErrA: Exception "
            "  public constructor(m: string) base(m); end constructor; "
            "end ErrA; "
            "class C public static method Main(): int "
            "  try "
            "    throw new ErrA(\"x\"); "
            "  catch var e: ErrA => return e.Message.Length; "
            "  end; "
            "  return 0; "
            "end Main; end C;",
            "no member"),
        false);   // must NOT have "no member" error
}

// ── Group 6: Regression — existing built-in exception Message still works ──────

// Regression guard: built-in exception Message path must still work correctly
// after the layout change.
// per §31 / CG22+.
BEZEL_TEST(codegen_595, regression_builtin_exception_message_still_works_spec_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(\"bad arg\"); "
        "  catch var e: ArgumentException => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        7);   // "bad arg".Length = 7
}

// Regression guard: no-arg builtin exception → empty Message.
// per §31 / CG22+.
BEZEL_TEST(codegen_595, regression_builtin_exception_no_message_spec_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new InvalidOperationException(); "
        "  catch var e: InvalidOperationException => return e.Message.Length; "
        "  end; "
        "  return -1; "
        "end Main; end C;"),
        0);   // empty message → Length 0
}
