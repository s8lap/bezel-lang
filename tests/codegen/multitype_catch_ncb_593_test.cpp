// Codegen tests for §31.2.1 multi-type catch arm binding — nearest common base
// (NCB). Issue #593: `var e: A, B => e.Member` must resolve against the NCB of
// A and B, not against the first listed type.
//
// §31.2.1 (verbatim): "When binding, `name` has the static type of the
// **nearest common base** of the listed types (the most specific type that is a
// base of every listed type; `Exception` if no more specific common base exists)."
//
// The tests are organized in four groups:
//
//   1. Two built-in types, NCB = built-in `Exception` — runtime-verifiable.
//      Both OverflowException and ArgumentException are built-in, so their NCB
//      (Exception) is also built-in, and e.Message works at runtime.
//
//   2. Two user types, NCB = built-in `Exception` — the exact issue #593 repro.
//      ErrA, ErrB both inherit Exception; e.Message resolves (no "no member"
//      sema error). Runtime: verify the arm catches; Message itself is not
//      testable at runtime for user exceptions (pre-existing architecture gap —
//      user exceptions don't inherit Exception's message-storage slot through the
//      current codegen's base-class layout mechanism).
//
//   3. Three user types, NCB = built-in `Exception` — extends case 2.
//
//   4. NCB is a user-declared class (not Exception) — both types share a
//      user-defined base. Tests that the `commonTypeOf`-fold path works for
//      user hierarchies (existing behavior, regression guard). Member access on
//      the user base class works at runtime.
//
//   5. Single-type arm regression — must still resolve exactly as before.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Returns true iff the run produced an error diagnostic containing `keyword`.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// ── Group 1: two built-in types, NCB = Exception (runtime-verifiable) ─────────

// OverflowException and ArgumentException are both built-in types; their NCB is
// built-in Exception.  e.Message is accessible because isBuiltinExceptionType()
// fires for any built-in Named TypeRef (declaration == nullptr).
// per §31.2.1: "Exception if no more specific common base exists."
BEZEL_TEST(codegen_593, two_builtin_types_message_resolves_spec_31_2_1) {
    // Throw OverflowException (first listed type). Catch via multi-type arm
    // with binding. Access .Message on the binding. "overflow".Length = 8.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new OverflowException(\"overflow\"); "
        "  catch "
        "    var e: OverflowException | ArgumentException => "
        "      return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        8);  // "overflow".Length = 8
}

// Throw ArgumentException (second listed type). Arm still catches.
// per §31.2.1: binding type is determined statically by the NCB rule.
BEZEL_TEST(codegen_593, two_builtin_types_second_thrown_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(\"arg\"); "
        "  catch "
        "    var e: OverflowException | ArgumentException => "
        "      return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        3);  // "arg".Length = 3
}

// ── Group 2: two user types, NCB = built-in Exception (#593 repro) ────────────

// The exact repro from #593: `var e: ErrA, ErrB` should bind `e` at the NCB
// static type (`Exception`) so `e.Message` resolves without "no member" error.
// Before the fix this emitted: "no member `Message` on type `ErrA`"
// per §31.2.1 (NCB = Exception when no more specific common base exists).
BEZEL_TEST(codegen_593, two_user_types_no_member_error_spec_31_2_1) {
    // Must NOT emit a "no member" sema error.
    BEZEL_EXPECT_EQ(
        hasError(
            "module M; "
            "class ErrA: Exception "
            "  public constructor(msg: string, code: int) end constructor; "
            "end ErrA; "
            "class ErrB: Exception "
            "  public constructor(msg: string) end constructor; "
            "end ErrB; "
            "class C public static method Main(): int "
            "  try "
            "    throw new ErrA(\"x\", 42); "
            "  catch "
            "    var e: ErrA | ErrB => return e.Message.Length; "
            "  end; "
            "  return 0; "
            "end Main; end C;",
            "no member"),
        false);  // must NOT have "no member" error
}

// Verify the catch arm actually executes (arm matches ErrA, body runs).
// Uses a constant return value so we don't need to access e.Message.
// per §31.2.1 / §31.2.3: first matching arm wins.
BEZEL_TEST(codegen_593, two_user_types_arm_executes_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(msg: string, code: int) end constructor; "
        "end ErrA; "
        "class ErrB: Exception "
        "  public constructor(msg: string) end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"a\", 42); "
        "  catch "
        "    var e: ErrA | ErrB => return 77; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        77);
}

// Second listed type thrown, arm still catches.
// per §31.2.1: the arm matches any of the listed types.
BEZEL_TEST(codegen_593, two_user_types_second_type_thrown_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(msg: string, code: int) end constructor; "
        "end ErrA; "
        "class ErrB: Exception "
        "  public constructor(msg: string) end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrB(\"b\"); "
        "  catch "
        "    var e: ErrA | ErrB => return 88; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        88);
}

// ── Group 3: three user types, NCB = built-in Exception ──────────────────────

// Three types all inheriting from Exception; NCB must be Exception.
// per §31.2.1: "the most specific type that is a base of every listed type."
BEZEL_TEST(codegen_593, three_user_types_no_member_error_spec_31_2_1) {
    // Must NOT emit a "no member" sema error.
    BEZEL_EXPECT_EQ(
        hasError(
            "module M; "
            "class ErrA: Exception "
            "  public constructor(msg: string) end constructor; "
            "end ErrA; "
            "class ErrB: Exception "
            "  public constructor(msg: string) end constructor; "
            "end ErrB; "
            "class ErrC: Exception "
            "  public constructor(msg: string) end constructor; "
            "end ErrC; "
            "class C public static method Main(): int "
            "  try "
            "    throw new ErrB(\"ab\"); "
            "  catch "
            "    var e: ErrA | ErrB | ErrC => return e.Message.Length; "
            "  end; "
            "  return 0; "
            "end Main; end C;",
            "no member"),
        false);  // must NOT have "no member" error
}

// Three-type arm: arm executes (catch matches the thrown type).
// per §31.2.1 / §31.2.3.
BEZEL_TEST(codegen_593, three_user_types_arm_executes_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(msg: string) end constructor; "
        "end ErrA; "
        "class ErrB: Exception "
        "  public constructor(msg: string) end constructor; "
        "end ErrB; "
        "class ErrC: Exception "
        "  public constructor(msg: string) end constructor; "
        "end ErrC; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrC(\"c\"); "
        "  catch "
        "    var e: ErrA | ErrB | ErrC => return 55; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        55);
}

// ── Group 4: NCB is a user-declared class (not Exception) ────────────────────

// Two types sharing a user-defined base class (not a built-in). The NCB is
// the user class. per §31.2.1: NCB = "most specific type that is a base of
// every listed type."
// Tests that `commonTypeOf` fold works for user hierarchies and that member
// access on the user NCB class resolves and executes at runtime.
BEZEL_TEST(codegen_593, ncb_is_user_class_member_resolves_spec_31_2_1) {
    // AppErr declares a virtual Code(): int method.
    // AppErrA and AppErrB both inherit AppErr.
    // var e: AppErrA, AppErrB → NCB = AppErr → e.Code() resolves.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppErr: Exception "
        "  public constructor(msg: string, code: int) "
        "    _code := code; "
        "  end constructor; "
        "  private _code: int; "
        "  public virtual method Code(): int return _code; end Code; "
        "end AppErr; "
        "class AppErrA: AppErr "
        "  public constructor(msg: string, code: int) "
        "    base(msg, code); "
        "  end constructor; "
        "end AppErrA; "
        "class AppErrB: AppErr "
        "  public constructor(msg: string, code: int) "
        "    base(msg, code); "
        "  end constructor; "
        "end AppErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new AppErrA(\"test\", 42); "
        "  catch "
        "    var e: AppErrA | AppErrB => return e.Code(); "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        42);
}

// ── Group 5: single-type arm regression guard ─────────────────────────────────

// Single-type binding arm with a built-in exception must still work exactly
// as before. per §31.2.1: a single-type arm's binding is the type itself.
BEZEL_TEST(codegen_593, single_builtin_type_arm_regression_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(\"bad\"); "
        "  catch "
        "    var e: ArgumentException => return e.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        3);  // "bad".Length = 3
}

// Single-type binding arm with a user exception. The arm must catch the
// exception and the body must execute. per §31.2.1.
BEZEL_TEST(codegen_593, single_user_type_arm_catches_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class MyErr: Exception "
        "  public constructor(msg: string) end constructor; "
        "end MyErr; "
        "class C public static method Main(): int "
        "  try "
        "    throw new MyErr(\"test\"); "
        "  catch "
        "    var e: MyErr => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        99);
}

// ── Group 6 (issue #976): System.IO / System.Threading exception families ─────
//
// stdlib §7.4 (TimeoutException) and §7.5 (IOException, FileNotFoundException,
// DirectoryNotFoundException, EndOfStreamException, PathTooLongException — all
// derive from IOException, which derives from Exception). These were
// unresolvable type names before #976; now they resolve as built-in exceptions
// (no import-gating yet, matching KeyNotFoundException) so they work as catch-arm
// type tests, `throw new` targets, and base classes.

// A bare IOException catch with .Message access (built-in → message storage
// works at runtime). "disk gone".Length == 9.
BEZEL_TEST(codegen_976, io_exception_caught_message_spec_7_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.IO; class C public static method Main(): int "
        "  try throw new IOException(\"disk gone\"); "
        "  catch var ex: IOException => return ex.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        9);
}

// catch IOException matches a thrown subtype (FileNotFoundException is-a
// IOException via the descriptor base-chain).
BEZEL_TEST(codegen_976, io_base_catches_subtype_spec_7_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.IO; class C public static method Main(): int "
        "  try throw new FileNotFoundException(\"nf\"); "
        "  catch var ex: IOException => return 7; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        7);
}

// A sibling type must NOT cross-catch: a thrown DirectoryNotFoundException is
// not a FileNotFoundException, so it propagates to the outer IOException arm.
BEZEL_TEST(codegen_976, io_siblings_do_not_cross_catch_spec_7_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.IO; class C public static method Main(): int "
        "  try "
        "    try throw new DirectoryNotFoundException(\"d\"); "
        "    catch var ex: FileNotFoundException => return 1; "  // must NOT fire
        "    end; "
        "  catch var ex: IOException => return 20; "             // fires
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        20);
}

// System.Threading TimeoutException resolves and is catchable (§7.4).
BEZEL_TEST(codegen_976, timeout_exception_caught_spec_7_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; class C public static method Main(): int "
        "  try throw new TimeoutException(\"late\"); "
        "  catch var ex: TimeoutException => return ex.Message.Length; "  // 4
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        4);
}

// A user class deriving from a built-in IO exception is catchable as IOException.
BEZEL_TEST(codegen_976, user_class_inherits_io_exception_spec_7_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.IO; "
        "class MyIoErr: IOException "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end MyIoErr; "
        "class C public static method Main(): int "
        "  try throw new MyIoErr(\"custom\"); "
        "  catch var ex: IOException => return 33; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        33);
}
