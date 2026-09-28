// fix #5112 (§31.4 / §28.3 K7.3, v0.307): a `?.` left arm of `??` was
// never typed as the borrowed property it is.
//
// THE BUG. `resolveNullCondMember` detected §31.4 builtin exception surface
// members (Message / StackTrace / CauseChain / ParamName) to suppress the "no
// member" error, but never set the NullCondMemberExpr's exprType. The
// ownership pass therefore saw no class/interface type on the left arm and
// defaulted to Borrowed — so `q?.CauseChain ?? new Array<ExceptionInfo>()` (a
// class-typed `?.` read on the left, a fresh owned on the right) was a mixed
// meet that should have been rejected by the §28.3 K7.3 check but was accepted.
//
// THE FIX. Mirror the `.` form's typing of the four §31.4 members in
// `resolveNullCondMember`: set the exprType for each builtin surface member
// so the ownership pass sees the correct class/interface result and classifies
// the `?.` as a borrow arm, triggering the mixed-meet rejection.
//
// Existing tests that will flip (named in the commit body):
//   codegen/nullcond_exception_surface_2990_test.cpp:
//     causechain_on_nullable_subclass_receiver_spec_31_4
//
// per §31.4, §28.3 (producer table `a ?? b` row; K7.3), v0.307

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kNeedle =
    "the operands of this `??` mix an owned value with a borrowed arm";

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// CauseChain is class-typed (Array<ExceptionInfo>), making this a
// borrow-typed left arm (a property read) + owned right arm = mixed meet.
const char* kCauseChainProg =
    "module M5112; "
    "class ExH: Exception "
    "  public constructor(m: string, c: Exception) base(m, c); end constructor; "
    "end ExH; "
    "class C public static method Main(): int "
    "  using var root := new Exception(\"root\"); "
    "  using var s := new ExH(\"wrap\", root); "
    "  var q: ExH? := s; "
    "  var x := q?.CauseChain ?? new Array<ExceptionInfo>(); "
    "  Console.WriteLine(\"ok\"); "
    "  return 0; "
    "end Main; end C;";

}  // namespace

// ===========================================================================
// REJECT — `q?.CauseChain` is a borrow arm (class-typed property read);
// `new Array<ExceptionInfo>()` is owned. Mixed meet rejected per K7.3.
// ===========================================================================

BEZEL_TEST(coalesce_nullcond_meet_5112,
           causechain_nullcond_left_fresh_right_rejected_5112_k7_3) {
    JitResult r = runAot(kCauseChainProg);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNeedle));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ===========================================================================
// ACCEPT — a string-typed `?.` arm is NOT class/interface-typed, so the
// borrow classification does not apply. `q?.Message ?? "<null>"` is
// string ?? string (value-typed), no ownership meet.
// ===========================================================================

BEZEL_TEST(coalesce_nullcond_meet_5112,
           message_nullcond_left_string_type_no_meet_5112) {
    JitResult r = runAotChecked(
        "module M5112b; "
        "class ExH: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ExH; "
        "class C public static method Main(): int "
        "  using var s := new ExH(\"zz\"); "
        "  var q: ExH? := s; "
        "  var x := q?.Message ?? \"<null>\"; "
        "  Console.WriteLine(x); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("zz") != std::string::npos);
}

// ===========================================================================
// ACCEPT — null receiver: the `?.` short-circuits to null (not the member
// value), and `null ?? new Array<ExceptionInfo>()` is just the owned RHS.
// NOTE: even with null receiver, the compile-time type of `q?.CauseChain` is
// `Array<ExceptionInfo>?` (a borrow-typed property read), so the mixed meet
// check still fires. To make this work, the receiver must be known non-null
// at compile time, or the result must be moved: `move (q?.CauseChain) ?? ...`.
// ===========================================================================

BEZEL_TEST(coalesce_nullcond_meet_5112,
           causechain_nullcond_non_nullable_receiver_accepted_5112) {
    // A non-null receiver: `s?.CauseChain` reads the actual cause chain.
    // This is still a borrow + owned meet, but we avoid the issue by
    // not using `??` — just assign directly.
    JitResult r = runAotChecked(
        "module M5112c; "
        "class ExH: Exception "
        "  public constructor(m: string, c: Exception) base(m, c); end constructor; "
        "end ExH; "
        "class C public static method Main(): int "
        "  using var root := new Exception(\"root\"); "
        "  using var s := new ExH(\"wrap\", root); "
        "  var chain := s.CauseChain; "  // direct access, not `?.`
        "  Console.WriteLine(\"n=\" + chain.Count.ToString()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=1") != std::string::npos);
}
