// Codegen — the §31.4 exception surface (v0.162/v0.163) and the #811 CauseChain
// snapshot-chaining ruling.
//
// §31.4 declares, for the first time, the base `Exception` surface:
//   constructor(); constructor(message); constructor(message, cause);
//   Message: string; StackTrace: string ("" when unavailable);
//   CauseChain: Array<ExceptionInfo>   (empty when no cause)
//   record ExceptionInfo(TypeName: string, Message: string, StackTrace: string)
// and, for ArgumentException:
//   constructor(message); constructor(message, paramName);
//   ParamName: string?   (null when not supplied).
//
// #811 (adopted in v0.162): cause chaining is BY SNAPSHOT. The (message, cause)
// constructor BORROWS the cause and copies its type-name, message, stack-trace
// text, and its own CauseChain into the new exception's CauseChain (outermost
// cause first, flattened). The cause instance is never retained.
//
// These tests are JIT-observable via the int exit code (runJit). The uncaught-
// exception report format (§33.3) is covered by exception_uncaught_report_...
// which needs the native compile+run harness (exit(70) would kill the JIT).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── StackTrace stub (§31.4: "formatted; \"\" when unavailable") ──────────────

// Pre-debug-info, StackTrace is the empty string. Its Length is therefore 0.
BEZEL_TEST(codegen_exc_surface, stacktrace_stub_empty_spec_31_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var e := new Exception(\"boom\"); "
        "  return e.StackTrace.Length; "
        "end Main; end C;"), 0);
}

// StackTrace is typed `string`; it composes with string ops without an
// ownership/borrow obligation (same treatment as Message).
BEZEL_TEST(codegen_exc_surface, stacktrace_is_string_spec_31_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var e := new InvalidOperationException(\"x\"); "
        "  var s: string := e.StackTrace; "
        "  return s.Length + 7; "
        "end Main; end C;"), 7);
}

// ── ArgumentException.ParamName (§31.4) ──────────────────────────────────────

// The (message, paramName) constructor records paramName; ParamName returns it.
BEZEL_TEST(codegen_exc_surface, argument_paramname_present_spec_31_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var e := new ArgumentException(\"bad\", \"amount\"); "
        "  return e.ParamName!.Length; "  // len("amount") == 6
        "end Main; end C;"), 6);
}

// §31.4: ParamName is `string?` and is null when not supplied (the single-arg
// (message) constructor).
BEZEL_TEST(codegen_exc_surface, argument_paramname_null_when_absent_spec_31_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var e := new ArgumentException(\"bad\"); "
        "  var p := e.ParamName; "
        "  if p is null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The paramName value round-trips (content, not just length).
BEZEL_TEST(codegen_exc_surface, argument_paramname_value_spec_31_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var e := new ArgumentException(\"bad\", \"amount\"); "
        "  var p: string := e.ParamName!; "
        "  if p = \"amount\" then return 42; end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// ── CauseChain snapshot (§31.4 / #811) ───────────────────────────────────────

// No cause → CauseChain is empty (Count 0).
BEZEL_TEST(codegen_exc_surface, cause_chain_empty_no_cause_spec_31_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var e := new Exception(\"solo\"); "
        "  return e.CauseChain.Count; "
        "end Main; end C;"), 0);
}

// One cause → CauseChain has exactly one record.
BEZEL_TEST(codegen_exc_surface, cause_chain_single_count_spec_811) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var e := new Exception(\"outer\", new ArgumentException(\"inner\")); "
        "  return e.CauseChain.Count; "
        "end Main; end C;"), 1);
}

// The snapshot copies the cause's TYPE NAME (outermost cause first).
// Post-#3119: built-in exception classes render as "System.<Name>" per
// §8.5/§2.13 — the same module-path-qualified rendering the stdlib defines
// for `Type.FullName`. #811's BY-SNAPSHOT ruling still pins the property
// (the cause's TypeName IS captured); only the exact rendering string
// moved to the qualified form.
BEZEL_TEST(codegen_exc_surface, cause_chain_typename_spec_811) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var e := new Exception(\"outer\", new ArgumentException(\"inner\")); "
        "  if e.CauseChain[0].TypeName = \"System.ArgumentException\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The snapshot copies the cause's MESSAGE text.
BEZEL_TEST(codegen_exc_surface, cause_chain_message_spec_811) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var e := new Exception(\"outer\", new InvalidOperationException(\"inner-msg\")); "
        "  if e.CauseChain[0].Message = \"inner-msg\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// #811: the chain is FLATTENED — the new exception's CauseChain is
// [info(cause)] ++ cause.CauseChain, outermost cause first. Nesting three deep
// (a caused-by b caused-by c) gives A.CauseChain == [b, c] (Count 2).
BEZEL_TEST(codegen_exc_surface, cause_chain_flattened_spec_811) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var c := new ArgumentException(\"c\"); "
        "  using var b := new Exception(\"b\", c); "
        "  using var a := new Exception(\"a\", b); "
        "  return a.CauseChain.Count; "  // [b, c]
        "end Main; end C;"), 2);
}

// #811: flattened order is outermost-first — a.CauseChain[0] is b, [1] is c.
BEZEL_TEST(codegen_exc_surface, cause_chain_flattened_order_spec_811) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var c := new ArgumentException(\"c\"); "
        "  using var b := new Exception(\"b\", c); "
        "  using var a := new Exception(\"a\", b); "
        "  var ok := 0; "
        "  if a.CauseChain[0].Message = \"b\" then ok := ok + 1; end; "
        "  if a.CauseChain[1].Message = \"c\" then ok := ok + 10; end; "
        "  return ok; "
        "end Main; end C;"), 11);
}

// ExceptionInfo.StackTrace is the snapshotted (stub) trace — "" pre-debug-info.
BEZEL_TEST(codegen_exc_surface, cause_chain_stacktrace_stub_spec_811) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var e := new Exception(\"outer\", new ArgumentException(\"inner\")); "
        "  return e.CauseChain[0].StackTrace.Length; "
        "end Main; end C;"), 0);
}

// ── Wrap-and-rethrow from a catch arm (§31.2.4 / #811 / #430 relaxation) ─────

// The spec's own §31.2.4 example: `throw new Wrapper("...", ex)` inside a catch
// arm, where ex is the caught borrow. The cause ctor SNAPSHOTS (retains
// nothing), so the #430 escape rejection must not fire; the outer catch sees
// the flattened chain. Was rejected before the cause-position relaxation
// ("caught exception may not be passed to a constructor...").
BEZEL_TEST(codegen_exc_surface, wrap_and_rethrow_in_catch_arm_spec_31_2_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static method Inner(): int "
        "  throw new ArgumentException(\"bad arg\", \"size\"); "
        "end Inner; "
        "public static method Main(): int "
        "  var pn := \"\"; var cnt := 0; var msg := \"\"; var tn := \"\"; "
        "  try "
        "    try "
        "      var r := C.Inner(); "
        "    catch e: ArgumentException => "
        "      do "
        "        pn := e.ParamName ?? \"\"; "
        "        throw new Exception(\"wrapped\", e); "
        "      end; "
        "    end; "
        "  catch e2: Exception => "
        "    do "
        "      cnt := e2.CauseChain.Count; "
        "      msg := e2.CauseChain[0].Message; "
        "      tn := e2.CauseChain[0].TypeName; "
        "    end; "
        "  end; "
        "  var ok := 0; "
        "  if pn = \"size\" then ok := ok + 1; end; "
        "  if cnt > 0 then ok := ok + 10; end; "
        "  if msg = \"bad arg\" then ok := ok + 100; end; "
        // fix #3119: CauseChain[0].TypeName snapshot is now the QUALIFIED
        // module path — "System.ArgumentException" (was "ArgumentException").
        "  if tn = \"System.ArgumentException\" then ok := ok + 1; end; "
        "  return ok; "  // 112 = all four
        "end Main; end C;"), 112);
}

// A caught exception passed to a NON-exception ctor in an escaping position is
// still rejected (#430 unchanged outside the cause position) — pinned by the
// sema suite; here pin the legal side: a user exception subclass chaining via
// base(msg, cause) from a catch arm is accepted and chains.
BEZEL_TEST(codegen_exc_surface, user_subclass_wrap_rethrow_spec_31_2_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception "
        "  public constructor(msg: string, cause: Exception) "
        "    base(msg, cause); "
        "  end constructor; "
        "end AppError; "
        "class C "
        "public static method Main(): int "
        "  var tn := \"\"; "
        "  try "
        "    try "
        "      throw new InvalidOperationException(\"root\"); "
        "    catch e: InvalidOperationException => "
        "      throw new AppError(\"ctx\", e); "
        "    end; "
        "  catch e2: Exception => "
        "    tn := e2.CauseChain[0].TypeName; "
        "  end; "
        "  if tn = \"System.InvalidOperationException\" then return 5; end; "   // #3119
        "  return 0; "
        "end Main; end C;"), 5);
}
