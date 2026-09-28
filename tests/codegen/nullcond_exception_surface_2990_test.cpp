// Codegen — issue #2990. `q?.M` on a nullable exception-typed receiver, where
// M is one of the four §31.4 header members, SILENTLY DROPPED the whole
// enclosing statement — no diagnostic at any severity, exit 0. The `start` and
// `end` markers printed but the middle line (the access and its effect)
// vanished.
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
// §12.1 makes a base class's members available on a derived class. §7.5
// (`?.`) lifts the result to nullable: a null receiver yields null, else the
// member value. v0.240.
//
// THE DEFECT. The four §31.4 members carry NO storage FieldSymbol — the `.`
// form reads them through byte-offset header GEPs in the MemberAccessExpr arm
// of lowerExpr (`ma->member == "Message"` and `isInheritedExcSurfaceAccess`).
// That arm never runs for a `?.` access: a NullCondMemberExpr is a distinct
// AST node, so the access reached the storage-field path of the
// NullCondMemberExpr arm, `storageMemberOf` returned null, and the `!ms`
// guard returned nullptr — silently dropping the enclosing statement. The
// caller (a statement-position expression) substitutes a degrade, which for a
// `Console.WriteLine(...)` is to omit the call entirely. MEASURED on 8eab3f8:
// `q?.Message ?? "<null>"` on a NON-NULL receiver carrying `Message = "zz"`
// printed nothing at all; `start` and `end` printed around the gap, exit 0.
//
// This is #2972's failure mode in a receiver shape #2972 did not cover: #2972
// was the `this`/`base` receiver (no exprType at all); #2971 was the member
// LOOKUP through a subclass's own static type. This is a `?.` receiver whose
// exprType IS present (`ExH?`, a Named TypeRef with a declaration and
// `nullable` set). The drop is purely in the lowering.
//
// THE FIX routes the `?.` form through the SAME header reads the `.` form
// uses, gated by the SAME receiver-type predicates (`isBuiltinException
// TypeRef` / `hasBuiltinExceptionBase`) — the one fact sema already computes
// that decides how the access lowers — with a null-receiver short-circuit
// first per §7.5 (the always-nullable result is a pointer that is null when
// the receiver is null). See LlvmBackend_Exprs.cpp, NullCondMemberExpr arm.
//
// Every case below asserts the program's OUTPUT BYTES. "It compiled" and
// "exit 0" are not evidence for a silent-drop bug: without the fix every
// positive case here compiles clean and exits 0 while printing the WRONG
// thing (nothing, or a line missing from the middle of the output).
//
// Mechanical fail-without: see the commit message. With `src/` reverted to
// 8eab3f8 and this file kept, every positive case below fails (the `?.` line
// is dropped, so the expected output does not match) and the negative control
// passes — it is a control precisely because it does not depend on the fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

bool prints(const std::string& program, const std::string& expected) {
    return runAotOut(program) == expected;
}

// `class ExH2990: Exception` with a (message, cause) chaining constructor, so
// its §31.4 CauseChain slot is POPULATED — an all-zero/null read is
// distinguishable from a correct one, which an empty chain would not be.
std::string chainProg(const std::string& members, const std::string& main) {
    return "module M2990; "
           "class ExH2990: Exception "
           "  public constructor(m: string, c: Exception) base(m, c); "
           "  end constructor; " +
           members +
           "end ExH2990; "
           "class C public static method Main(): int "
           "  using var root := new Exception(\"root\"); "
           "  using var s := new ExH2990(\"wrap\", root); "
           "  var q: ExH2990? := s; " +
           main +
           "  return 0; "
           "end Main; end C;";
}

// fix #5112 (§28.3 K7.3, v0.307): a `q?.CauseChain ?? new Array<ExceptionInfo>()`
// pair is a mixed borrow/owned meet. Pre-fix the `?.` form had no exprType for
// §31.4 surface members so the meet check never fired; post-fix it fires and
// the program is REJECTED. These two rows previously asserted ACCEPT with the
// expected output bytes; they now assert the specific K7.3 diagnostic.
const char* kMeetNeedle =
    "the operands of this `??` mix an owned value with a borrowed arm";

bool hasMeetError(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kMeetNeedle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// A. The issue's own repro. `start`/`end` markers bracket the access: without
// the fix the middle `qm=` line is dropped entirely (the whole WriteLine call
// vanishes), so the output is "start\nend". With the fix it is the three-line
// "start\nqm=zz\nend". The receiver is non-null and carries Message = "zz".
// ===========================================================================

BEZEL_TEST(codegen_nullcond_exception_surface_2990,
           message_on_nullable_subclass_receiver_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2990a; "
        "class ExH2990: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ExH2990; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"start\"); "
        "  using var s := new ExH2990(\"zz\"); "
        "  var q: ExH2990? := s; "
        "  Console.WriteLine(\"qm=\" + (q?.Message ?? \"<null>\")); "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;",
        "start\nqm=zz\nend"));
}

// ===========================================================================
// B. All four §31.4 members through `?.` on a nullable subclass receiver.
//
// `StackTrace` is §31.4's ""-when-unavailable stub, so it is asserted inside
// brackets: an empty read and a degraded/absent read print identically without
// them. CauseChain uses the populated (message, cause) constructor above.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_exception_surface_2990,
           stacktrace_on_nullable_subclass_receiver_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        chainProg("",
                  "  Console.WriteLine(\"st=[\" + (q?.StackTrace ?? \"<null>\") + \"]\"); "),
        "st=[]"));
}

BEZEL_TEST(codegen_nullcond_exception_surface_2990,
           causechain_on_nullable_subclass_receiver_spec_31_4) {
    // fix #5112 (§28.3 K7.3, v0.307): mixed meet — REJECT.
    bezel::test::JitResult r = runAot(chainProg(
        "",
        "  Console.WriteLine(\"n=\" + (q?.CauseChain ?? new Array<ExceptionInfo>()).Count.ToString()); "
        "  Console.WriteLine(\"m=\" + (q?.CauseChain ?? new Array<ExceptionInfo>())[0].Message); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasMeetError(r));
}

// §31.4 "empty when no cause" through `?.`: a subclass built through the
// one-argument base constructor leaves the slot calloc-zeroed, so the chain
// reads EMPTY (Count = 0), not null and not faulting.
BEZEL_TEST(codegen_nullcond_exception_surface_2990,
           causechain_empty_through_nullable_spec_31_4) {
    // fix #5112 (§28.3 K7.3, v0.307): mixed meet — REJECT.
    bezel::test::JitResult r = runAot(
        "module M2990b; "
        "class Plain2990: Exception "
        "  public constructor() base(\"mm\"); end constructor; "
        "end Plain2990; "
        "class C public static method Main(): int "
        "  using var s := new Plain2990(); "
        "  var q: Plain2990? := s; "
        "  Console.WriteLine(\"n=\" + (q?.CauseChain ?? new Array<ExceptionInfo>()).Count.ToString()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasMeetError(r));
}

// §31.4 `ParamName: string?` on an ArgumentException subclass through `?.`.
BEZEL_TEST(codegen_nullcond_exception_surface_2990,
           paramname_on_nullable_argument_subclass_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2990c; "
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
// C. A BUILT-IN-typed nullable receiver (`var e: Exception? := …; e?.M`). The
// issue noted this shares a cause; it is the `isBuiltinExceptionTypeRef` arm.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_exception_surface_2990,
           message_on_builtin_typed_nullable_receiver_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2990d; "
        "class C public static method Main(): int "
        "  using var s := new Exception(\"bi\"); "
        "  var e: Exception? := s; "
        "  Console.WriteLine(\"em=\" + (e?.Message ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "em=bi"));
}

// ===========================================================================
// D. §7.5 null-receiver short-circuit: a null receiver yields null, so the
// coalesce supplies the default. This is the arm that distinguishes `?.` from
// `.` and proves the null check runs at all (a path that read the header
// unconditionally would fault on the null receiver).
// ===========================================================================

BEZEL_TEST(codegen_nullcond_exception_surface_2990,
           null_receiver_short_circuits_to_null_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2990e; "
        "class ExH2990: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ExH2990; "
        "class C public static method Main(): int "
        "  var q: ExH2990? := null; "
        "  Console.WriteLine(\"qm=\" + (q?.Message ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "qm=<null>"));
}

// ===========================================================================
// E. NEGATIVE CONTROL — a class with NO built-in exception ancestor that
// declares its OWN `Message` storage field. The `?.` access must read the
// declared field through the existing storage-member path, NOT be intercepted
// by the §31.4 header reader: the receiver is not an exception type, so the
// new block declines and falls through to FINDING [3]. This passes WITHOUT the
// fix as well as with it; that is what makes it a control rather than a pin.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_exception_surface_2990,
           own_message_field_on_a_non_exception_class_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2990f; "
        "class NotEx2990 "
        "  public Message: string; "
        "  public constructor() Message := \"own-msg\"; end constructor; "
        "end NotEx2990; "
        "class C public static method Main(): int "
        "  using var n := new NotEx2990(); "
        "  var q: NotEx2990? := n; "
        "  Console.WriteLine(\"m=\" + (q?.Message ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "m=own-msg"));
}
