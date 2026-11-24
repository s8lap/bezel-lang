// Ownership — inside a LAMBDA body, an owned local that is moved on one path
// and left unmoved on the path that `return`s was admitted and LEAKED (100
// objects over 200 calls under LeakSanitizer); the byte-identical METHOD body
// is rejected. Issue #8939.
//
// SPEC: §28.3 "Local variables, ownership, and borrows" (v0.359), the owned
// local discharge requirement: every owned local must be discharged (`move`,
// `return`, `.Dispose()` or `using var`) on every path; K7.4: owned ⊓
// moved-from = maybe-consumed, which the scope-end disposal cannot settle for
// a class local, so the method body rejects it with
// "owned value in `x` would be dropped here without disposal (§28.3 ...)".
//
// ROOT CAUSE: lambda bodies are walked by a pre-pass
// (`applyUsingVarBorrowPinInLambdaStmt`) that has no end-of-body check, so the
// syntactic scope-end sweep skipped a local it had seen moved on SOME path
// (`everDischarged_`) and never armed its disposal on the unmoved path. The
// walker now joins the state at every `return` / the fall-off end and reports
// an owned own local left maybe-consumed, with the method twin's text.
//
// MEASURED PRE-FIX: compiled rc=0, no diagnostic; ASan: "Direct leak of 1600
// byte(s) in 100 object(s)".

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// The METHOD twin's exact diagnostic.
const char* kDropped =
    "owned value in `x` would be dropped here without disposal (§28.3";

template <class Diags>
bool hasDropped(const Diags& ds) {
    for (const auto& d : ds.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kDropped) != std::string::npos)
            return true;
    return false;
}

const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "end Sink; ";

// The issue's repro.
std::string ifMoveReturnUnmoved() {
    return std::string(kPrelude) +
           "class C "
           "  public static method F(flag: bool, n: int): int "
           "    var f := lambda(flag: bool, n: int): int => do "
           "      var x := new Res(1); "
           "      if flag then var k := Sink.Take(move x); return k; end; "
           "      return x.tag; "
           "    end; "
           "    return f(flag, n); "
           "  end F; "
           "  public static method Main(): int "
           "    var s := 0; var i := 0; "
           "    while i < 200 do "
           "      s := s + C.F(i mod 2 = 0, i mod 3); i := i + 1; "
           "    end; "
           "    Console.WriteLine(s); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// Fix-forward (non-author review of the first #8939 commit): lambdas whose
// every path discharges `x` but whose LAST statement `armNeverFallsThrough`
// cannot classify (a `case` / `try` whose arms all return, an unreachable
// `return` after an if/else that both return) must stay ADMITTED, exactly as
// on base and as the method twin admits them (§28.3: the discharge is
// required on every path, and here every path discharges).
std::string lambdaMain(const std::string& lam) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int "
           "    var f := " + lam +
           "    var s := 0; var i := 0; "
           "    while i < 300 do s := s + f(i mod 3); i := i + 1; end; "
           "    Console.WriteLine(s); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

BEZEL_TEST(lambda_body_conditionally_moved_local_returned_unmoved_8939,
           if_move_then_return_unmoved_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(ifMoveReturnUnmoved());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDropped(r.diagnostics));
}

BEZEL_TEST(lambda_body_conditionally_moved_local_returned_unmoved_8939,
           aot_if_move_then_return_unmoved_is_a_compile_error_28_3) {
    JitResult jr = runAot(ifMoveReturnUnmoved());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(hasDropped(jr.diagnostics));
}

// Fix-forward rows: each is REJECTED by the first #8939 commit (phantom
// fall-off exit) and ADMITTED with the fix-forward.
BEZEL_TEST(lambda_body_conditionally_moved_local_returned_unmoved_8939,
           case_all_arms_return_every_path_discharges_admitted_28_3) {
    JitResult jr = runAot(lambdaMain(
        "lambda(n: int): int => do "
        "  var x := new Res(n); "
        "  case n of "
        "    0 => do return Sink.Take(move x); end, "
        "    1 => do var t := x.tag; x.Dispose(); return t; end, "
        "    _ => do x.Dispose(); return -1; end "
        "  end case; "
        "end; "));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

BEZEL_TEST(lambda_body_conditionally_moved_local_returned_unmoved_8939,
           if_else_both_return_then_unreachable_return_admitted_28_3) {
    JitResult jr = runAot(lambdaMain(
        "lambda(n: int): int => do "
        "  var x := new Res(n); "
        "  if n = 0 then return Sink.Take(move x); "
        "  else x.Dispose(); return 1; end; "
        "  return 9; "
        "end; "));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

BEZEL_TEST(lambda_body_conditionally_moved_local_returned_unmoved_8939,
           try_catch_all_return_every_path_discharges_admitted_28_3) {
    JitResult jr = runAot(lambdaMain(
        "lambda(n: int): int => do "
        "  var x := new Res(n); "
        "  try "
        "    if n = 0 then return Sink.Take(move x); end; "
        "    x.Dispose(); return 1; "
        "  catch ArgumentException => do return 2; end, "
        "  end try; "
        "end; "));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}
