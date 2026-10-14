// Codegen — #6142 (§12.7 / §8.6 / §28.3, v0.338): a boxed primitive argument
// at an `Object` parameter is released after the call when the callee only
// reads the narrowed value through an operator.
//
// §8.6 (v0.338): a value type used where `Object` is expected is boxed, and
// the box minted for an argument is an unbound owned temporary that ends at
// the call return (§28.3). #2952 releases it there when the callee's
// parameter is provably non-escaping (ParamEscapeWalker). The walker counted
// every operator operand as an escape, so `if o is int then return o + 1;
// end if;` (§12.7's narrowing of a boxed value: the operator reads the box's
// payload and produces a fresh int) suppressed the release and each call
// leaked its 24-byte box.
//
// THE REPRO is the issue's own program (100 calls, 2400 bytes leaked before
// the fix, LeakSanitizer). The other accept rows cover the remaining operator
// shapes (unary `-`, `not`, `=` against a value) and the `when`-guard arm of
// a case expression (§7.4: "a `when` guard (§19.4, narrowing that arm's
// body)"), which is the spelling the #6082 guard narrowing made reachable.
//
// OVER-RELEASE GUARDS. A callee that ALSO passes the parameter to another
// call or stores it in a field must keep the box alive (the walker's escape
// verdict wins over the operand read). Those rows run under ASan with leak
// detection off, so a premature release surfaces as a use-after-free on the
// later read rather than as a missing leak.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The issue's repro, verbatim apart from the module name.
const char* kIssueRepro =
    "module M6142A; "
    "public class P "
    "  public static method T(o: Object): int "
    "    if o is int then return o + 1; end if; "
    "    return -9; "
    "  end T; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<100 do t := t + T(5); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "end P;";

const char* kUnaryMinus =
    "module M6142B; "
    "public class P "
    "  public static method T(o: Object): int "
    "    if o is int then return -o; end if; "
    "    return 0; "
    "  end T; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<100 do t := t + T(3); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "end P;";

const char* kNotAndEquality =
    "module M6142C; "
    "public class P "
    "  public static method U(o: Object): bool "
    "    if o is bool then return not o; end if; "
    "    return false; "
    "  end U; "
    "  public static method V(o: Object): int "
    "    if o is int then if o = 5 then return 1; end if; end if; "
    "    return 0; "
    "  end V; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<100 do "
    "      t := t + V(5); "
    "      if U(false) then t := t + 1; end if; "
    "    end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "end P;";

// The rv4 o02 shape: the operand read sits in a guarded arm of a case
// expression.
const char* kCaseWhenGuardArm =
    "module M6142D; "
    "public class P "
    "  public static method T(k: int, o: Object): int "
    "    var r := (case k of _ when o is int => o + 1, _ => -9); "
    "    return r; "
    "  end T; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<100 do t := t + T(0, 5); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "end P;";

// OVER-RELEASE GUARD 1: the operand read AND a pass to another call. The
// call is an escape, so the box is not released (it leaks, the safe
// direction #2952 records). 20 calls x (5 + 5).
const char* kReadAndPassOn =
    "module M6142E; "
    "public class P "
    "  public static method Inner(v: Object): int "
    "    return (v as int) ?? 0; "
    "  end Inner; "
    "  public static method T(o: Object): int "
    "    if o is int then return o + Inner(o); end if; "
    "    return 0; "
    "  end T; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<20 do t := t + T(5); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "end P;";

// OVER-RELEASE GUARD 2: the operand read AND a store into a field that
// outlives the call; the field is read after the call returns.
const char* kReadAndStoreInField =
    "module M6142F; "
    "public class Sink "
    "  public _saved: Object; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Store(v: Object): int "
    "    var n := 0; "
    "    if v is int then n := v + 1; end if; "
    "    _saved := v; "
    "    return n; "
    "  end Store; "
    "end Sink; "
    "public class P "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var a := s.Store(7); "
    "    var b := (s._saved as int) ?? 0; "
    "    Console.WriteLine($\"{a} {b}\"); "
    "    return 0; "
    "  end Main; "
    "end P;";

}  // namespace

// per §12.7 / §8.6 / §28.3, v0.338 — the issue's repro: the narrowed operand
// read releases the argument box after each call (LSan reported 2400 bytes
// before the fix).
BEZEL_TEST(codegen_narrowed_operand_box_release_6142,
           issue_repro_binary_operand_releases_box_spec_12_7) {
    JitResult plain = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("600\n"));
    JitResult r = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("600\n"));
}

// per §12.7 / §8.6, v0.338 — a unary operator reads the payload as well.
BEZEL_TEST(codegen_narrowed_operand_box_release_6142,
           unary_minus_operand_releases_box_spec_12_7) {
    JitResult plain = runAot(kUnaryMinus);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("-300\n"));
    JitResult r = runAotChecked(kUnaryMinus);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-300\n"));
}

// per §12.7 / §8.6, v0.338 — `not` over a boxed bool and `=` against a
// value read the payload.
BEZEL_TEST(codegen_narrowed_operand_box_release_6142,
           not_and_equality_operands_release_box_spec_12_7) {
    JitResult plain = runAot(kNotAndEquality);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("200\n"));
    JitResult r = runAotChecked(kNotAndEquality);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

// per §7.4 / §19.4 / §8.6, v0.338 — the read in a `when`-guarded arm of a
// case expression.
BEZEL_TEST(codegen_narrowed_operand_box_release_6142,
           case_when_guard_arm_operand_releases_box_spec_7_4) {
    JitResult plain = runAot(kCaseWhenGuardArm);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("600\n"));
    JitResult r = runAotChecked(kCaseWhenGuardArm);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("600\n"));
}

// per §28.3, v0.338 — the parameter also passes to another call, an escape:
// the box is kept (no use-after-free; leak detection off).
BEZEL_TEST(codegen_narrowed_operand_box_release_6142,
           operand_read_plus_pass_on_keeps_box_spec_28_3) {
    JitResult r = runAotCheckedLenient(kReadAndPassOn);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

// per §28.3, v0.338 — the parameter is also stored in a field read after
// the call: the box is kept (no use-after-free; leak detection off).
BEZEL_TEST(codegen_narrowed_operand_box_release_6142,
           operand_read_plus_field_store_keeps_box_spec_28_3) {
    JitResult r = runAotCheckedLenient(kReadAndStoreInField);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8 7\n"));
}
