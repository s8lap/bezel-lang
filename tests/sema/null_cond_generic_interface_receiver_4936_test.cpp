// fix #4936 (§7.5 / §14.1 / §7.10, v0.307): a null-conditional member-call
// `n?.Get()` on a nullable GENERIC INTERFACE receiver `n: I<int>?` was typed
// as the unresolved `?` instead of `int?`.  The plain `.` path
// (`b.Get()` on a class `Holder<int?>`) substituted correctly, so
// `Console.WriteLine(h?.Get() ?? -1)` printed; the `?.` form on a class
// receiver was fixed by opus-4919 (f4cb60c2) — extending the
// substituteMemberType gate from the `MemberAccessExpr` callee to a
// `NullCondMemberExpr` callee. The interface-receiver case sat on the
// same gate's other half: `substituteMemberType` is parameterized on
// `ms->parent`, which is the interface `I<T>`. The helper's
// `recvTy->declaration == parent && !recvTy->genericArgs.empty()` branch
// already constructs the `{T := int}` map for an interface-receiver
// instantiation — but until f4cb60c2 reached that callee type at all,
// the bare T was returned, the §25 `+` operator was "not defined for
// type `?`", and a downstream Console.WriteLine overload resolved
// ambiguously into malformed IR (§9: by-value record semantics on the
// bare-T return path).
//
// Already-fixed-by-f4cb60c2 / no additional source change required: this
// test file pins the INTERFACE half of f4cb60c2's substitution gate.
// Without f4cb60c2, every row that touches a `?.`-member-call on a
// generic interface receiver fails: row 1 with the IR-verifier abort
// the issue records, row 4 with the §25 "operator `+` is not defined
// for type `?`" semantic rejection.
//
// Rows:
//   (1) ACCEPT — the issue's exact repro: `n?.Get() ?? 0` on `I<int>?`.
//       Pre-fix FAILS (malformed IR on WriteLine); post-fix PASSES with
//       `5` printed.
//   (2) ACCEPT — control: non-generic interface (`method Get(): int`)
//       — the substitution helper's empty-genericParams early-out makes
//       this a no-op, so it accepts on both sides of the fix.
//   (3) ACCEPT — control: the plain `.` form on `c: I<int>` — the gate
//       f4cb60c2 was extending.
//   (4) ACCEPT — control: the issue's second variant
//       `var v := n?.Get() ?? 0; v + 1` — pins that the SEMANTIC return
//       type was also resolved (semantic acceptance, not just codegen).
//
// Per [[feedback-test-oracles]]: row 1 pins the runtime output (the
// codegen-correct `5`); row 4 pins both compile-time acceptance and
// runtime output (`6`) — the strict oracle the spec requires.
//
// Per [[feedback-reviewer-must-run-accept-rows]] ★★★ the ACCEPT rows
// drive `runAot` (the strict AOT oracle), not the JIT path.
//
// Per [[feedback-regression-pin-vs-authored-case]] ★★★ regression pins
// go in `tests/<area>/`; the test-name suffix matches the issue number
// so the fix-flip grep binds cleanly per [[feedback-testfile-and-
// suite-inherit-issue-number]].

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// (1) ACCEPT — the issue's exact repro. Pre-fix FAILS (WriteLine emits
// nothing on the bare-T return; sema surfaces the §25 "operator `+` is
// not defined for type `?`" on row 4); post-fix PASSES with `5` printed.
BEZEL_TEST(null_cond_generic_interface_receiver_4936,
           issue_repro_n_dot_Get_dq_0_on_I_intQ_prints_5) {
    auto r = runAot(
        "module M4936; "
        "public interface I4936<T> "
        "  method Get(): T; "
        "end I4936; "
        "public class C4936: I4936<int> "
        "  public constructor() end constructor; "
        "  public method Get(): int => 5; "
        "end C4936; "
        "public class P4936 "
        "  public static method Main(): int "
        "    using var c := new C4936(); "
        "    var n: I4936<int>? := c; "
        "    Console.WriteLine(n?.Get() ?? 0); "
        "    return 0; "
        "  end Main; "
        "end P4936;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// (2) ACCEPT — control: non-generic interface — substituteMemberType's
// empty-genericParams early-out yields the declared type unchanged.
// Both pre- and post-fix print `5`.
BEZEL_TEST(null_cond_generic_interface_receiver_4936,
           n_dot_Get_dq_0_on_non_generic_I_prints_5_control) {
    auto r = runAot(
        "module M4936ng; "
        "public interface I4936ng "
        "  method Get(): int; "
        "end I4936ng; "
        "public class C4936ng: I4936ng "
        "  public constructor() end constructor; "
        "  public method Get(): int => 5; "
        "end C4936ng; "
        "public class P4936ng "
        "  public static method Main(): int "
        "    using var c := new C4936ng(); "
        "    var n: I4936ng? := c; "
        "    Console.WriteLine(n?.Get() ?? 0); "
        "    return 0; "
        "  end Main; "
        "end P4936ng;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// (3) ACCEPT — control: plain `.` form on `n.Get()` where `n: I4936<int>`
// (non-nullable interface instantiation). The plain-`.` arm was
// already correctly substituting before f4cb60c2; this pins that the
// null-conditional-side extension did not regress the gate it extended.
BEZEL_TEST(null_cond_generic_interface_receiver_4936,
           plain_dot_Get_on_I_int_interface_receiver_prints_5_control) {
    auto r = runAot(
        "module M4936d; "
        "public interface I4936d<T> "
        "  method Get(): T; "
        "end I4936d; "
        "public class C4936d: I4936d<int> "
        "  public constructor() end constructor; "
        "  public method Get(): int => 5; "
        "end C4936d; "
        "public class P4936d "
        "  public static method Main(): int "
        "    using var c := new C4936d(); "
        "    var n: I4936d<int> := c; "
        "    Console.WriteLine(n.Get()); "
        "    return 0; "
        "  end Main; "
        "end P4936d;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// (4) ACCEPT — control: the issue's second variant
//   `var v := n?.Get() ?? 0; Console.WriteLine(v + 1);`
// Pre-fix REJECTS in sema with `operator + is not defined for type ?`.
// Post-fix ACCEPTS and prints `6`. This pins that the substitution site
// change ALSO fixed the semantic acceptance (return type resolved to
// int?), not just the runtime codegen arm.
BEZEL_TEST(null_cond_generic_interface_receiver_4936,
           v_plus_1_variant_accepts_and_prints_6_control) {
    auto r = runAot(
        "module M4936v; "
        "public interface I4936v<T> "
        "  method Get(): T; "
        "end I4936v; "
        "public class C4936v: I4936v<int> "
        "  public constructor() end constructor; "
        "  public method Get(): int => 5; "
        "end C4936v; "
        "public class P4936v "
        "  public static method Main(): int "
        "    using var c := new C4936v(); "
        "    var n: I4936v<int>? := c; "
        "    var v := n?.Get() ?? 0; "
        "    Console.WriteLine(v + 1); "
        "    return 0; "
        "  end Main; "
        "end P4936v;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

}  // namespace
