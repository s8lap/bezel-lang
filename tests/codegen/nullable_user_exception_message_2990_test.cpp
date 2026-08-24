// Codegen — issue #2990. `q?.M` on a nullable exception-typed receiver, where
// M is one of the four §31.4 header members, SILENTLY DROPPED the whole
// enclosing statement — no diagnostic at any severity, exit 0. The `start` and
// `end` markers printed but the middle line (the access and its effect)
// vanished.
//
// This is #2972's failure mode surviving in a receiver shape its fix did not
// cover: #2972 was the `this`/`base` receiver (no exprType at all); #2971 was
// the member LOOKUP through a subclass's own static type. This is a `?.`
// receiver whose exprType IS present (`ExH?`, a Named TypeRef with a
// declaration and `nullable` set). The drop is purely in the lowering.
//
// Spec: §31.4 "Standard exception types" —
//
//     public class Exception
//       public readonly property Message: string;
//       public readonly property StackTrace: string;             // "" when unavailable
//       public readonly property CauseChain: Array<ExceptionInfo>;   // empty when no cause
//     end Exception;
//     public class ArgumentException: Exception
//       public readonly property ParamName: string?;             // null when not supplied
//     end ArgumentException;
//
// §12.1 makes a base class's members available on a derived class. §7 lifts
// `q?.M` to an always-nullable result: a null receiver yields null, else the
// member value. v0.240.
//
// THE FIX routes the `?.` form through the SAME header reads the `.` form
// uses, gated by the SAME receiver-type predicates (`isBuiltinException
// TypeRef` / `hasBuiltinExceptionBase`) — the one fact sema already computes
// that decides how the access lowers — with a null-receiver short-circuit
// first per §7 (the always-nullable result is a pointer that is null when the
// receiver is null). See LlvmBackend_Exprs.cpp, NullCondMemberExpr arm.
//
// Every case below asserts the program's OUTPUT BYTES. "It compiled" and
// "exit 0" are not evidence for a silent-drop bug: without the fix every
// positive case here compiles clean and exits 0 while printing the WRONG
// thing (nothing, or a line missing from the middle of the output).
//
// Fail-without: every positive case below FAILS (the `?.` line is dropped, so
// the expected output does not match). The two controls pass without the fix
// as well — they are controls precisely because they do not depend on the fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

bool prints(const std::string& program, const std::string& expected) {
    return runAotOut(program) == expected;
}

// `class Ex2990: Exception` with a one-arg `base(m)` constructor; `Message`
// reads the `m` argument out of the §31.4 header slot at byte 8.
std::string msgOnlyProg(const std::string& main) {
    return "module M2990; "
           "class Ex2990: Exception "
           "  public constructor(m: string) base(m); end constructor; "
           "end Ex2990; "
           "class C public static method Main(): int "
           "  using var s := new Ex2990(\"zz\"); "
           "  var q: Ex2990? := s; " +
           main +
           "  return 0; "
           "end Main; end C;";
}

}  // namespace

// ===========================================================================
// A. The issue's own repro on the §31.4 Message surface. `start`/`end` markers
// bracket the access: without the fix the middle `qm=` line is dropped
// entirely (the whole WriteLine call vanishes), so the output is
// "start\nend". With the fix it is "start\nqm=zz\nend". The receiver is
// non-null and carries Message = "zz".
// ===========================================================================

BEZEL_TEST(codegen_nullable_user_exception_message_2990,
           message_on_nullable_user_subclass_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        msgOnlyProg(
            "  Console.WriteLine(\"start\"); "
            "  Console.WriteLine(\"qm=\" + (q?.Message ?? \"<null>\")); "
            "  Console.WriteLine(\"end\"); "),
        "start\nqm=zz\nend"));
}

// ===========================================================================
// B. The three sibling members — `StackTrace`, `CauseChain`, `ParamName` — on
// a nullable user-subclass receiver through `?.`. Same silent-drop failure
// mode: `StackTrace` is the "" stub (§31.4 "formatted; "" when unavailable")
// so the bare read prints "[]" once bracketed; `ParamName` requires an
// ArgumentException subclass; `CauseChain` is reached through `??` because
// §7 lifts `q?.CauseChain` to a nullable Array and a further `.Count` needs
// the non-null arm — chained `?.Count` after the §31.4 access is out of
// scope here.
// ===========================================================================

BEZEL_TEST(codegen_nullable_user_exception_message_2990,
           stacktrace_on_nullable_user_subclass_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        msgOnlyProg(
            "  Console.WriteLine(\"st=[\" + (q?.StackTrace ?? \"<null>\") + \"]\"); "),
        "st=[]"));
}

BEZEL_TEST(codegen_nullable_user_exception_message_2990,
           causechain_on_nullable_user_subclass_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        msgOnlyProg(
            "  Console.WriteLine(\"n=\" + (q?.CauseChain ?? new Array<ExceptionInfo>()).Count.ToString()); "),
        "n=0"));
}

BEZEL_TEST(codegen_nullable_user_exception_message_2990,
           paramname_on_nullable_argument_subclass_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2990p; "
        "class ArgSub2990: ArgumentException "
        "  public constructor(m: string, p: string) base(m, p); end constructor; "
        "end ArgSub2990; "
        "class C public static method Main(): int "
        "  using var s := new ArgSub2990(\"mm\", \"pp\"); "
        "  var q: ArgSub2990? := s; "
        "  Console.WriteLine(\"pn=\" + (q?.ParamName ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "pn=pp"));
}

// ===========================================================================
// C. Built-in-typed nullable receiver (`var e: Exception? := …; e?.M`). Same
// silent-drop failure mode on the `isBuiltinExceptionTypeRef` arm of the
// fix. This is the case the issue called out as "shares a cause".
// ===========================================================================

BEZEL_TEST(codegen_nullable_user_exception_message_2990,
           message_on_builtin_typed_nullable_receiver_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2990b; "
        "class C public static method Main(): int "
        "  using var s := new Exception(\"bi\"); "
        "  var e: Exception? := s; "
        "  Console.WriteLine(\"em=\" + (e?.Message ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "em=bi"));
}

// ===========================================================================
// D. CONTROL — a NON-nullable `q.Message` (no `?.`) on a user exception
// subclass. This is the `.` form on the same receiver shape, which has
// always read the §31.4 header slot via the older `ma->member == "Message"`
// handler. It must still work, both with and without the new `?.` arm of
// the fix: this passes WITHOUT the fix as well, and that is what makes it
// a control rather than a pin — it confirms the `.` form is unaffected.
// ===========================================================================

BEZEL_TEST(codegen_nullable_user_exception_message_2990,
           non_nullable_dot_on_user_subclass_control_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2990c; "
        "class Ex2990: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end Ex2990; "
        "class C public static method Main(): int "
        "  using var s := new Ex2990(\"zz\"); "
        "  Console.WriteLine(\"dm=\" + s.Message); "
        "  return 0; "
        "end Main; end C;",
        "dm=zz"));
}
