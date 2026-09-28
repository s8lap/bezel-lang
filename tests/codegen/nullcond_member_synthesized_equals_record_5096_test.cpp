// fix #5096 (§9.1 / §8.3 / stdlib §2.9, v0.307): `np?.Equals(x)` on a
// nullable record that LISTS `IEquatable<Self>` emitted malformed IR.
//
// §9.1: "the synthesized `Equals(T)` is what satisfies a declared
// `IEquatable<T>` conformance". The synthesized witness has no `MethodSymbol`,
// so once a record lists `IEquatable<Self>` (made compilable by #5048) sema
// binds `np?.Equals(x)` to the abstract builtin `IEquatable<T>.Equals`. #5068
// fixed the `.` (MemberAccessExpr) spelling of the same call: it taught
// `tryLowerValueSemanticsCall` that the abstract `IEquatable.Equals`
// MethodSymbol paired with an auto-bearing-record receiver means "synthesized
// Equals witness — route to structural equality, not to interface dispatch".
//
// #5096 is the `?.` (NullCondMemberExpr) spelling of the SAME program. The callee
// is `NullCondMemberExpr`, not `MemberAccessExpr`, so the .`tryLowerValueSemanticsCall`
// gate at the top of the helper (`auto* ma = dynamic_cast<...>(call.callee.get());
// if (!ma || !ma->target) return nullptr;`) returns nullptr at line 2799 and the
// fix never reaches the synthesized-Equals detection added at line 2821. The
// dispatch then falls into `lowerNullCondCall`'s general ms-driven path, which
// tries to dispatch the abstract `IEquatable.Equals` on a record-VALUE receiver
// and bails partway through, leaving `nullcall.end` unterminated:
//
//     Basic Block in function 'main' does not have terminator!
//     label %nullcall.end
//     bezelc: codegen error: IR verification failed (malformed IR emitted...)
//
// MEASURED on r23-c28 (7367d60b): `bezelc probe.bzl -o probe.out` exits 1 with
// the verifier error above. The control program with an unlisted record
// compiles and runs, exiting 1 (the `if X ?? false then r := 1; end if` arm
// does not fire because the synthesized Equals returns false for unequal
// values `Pt(1,2)` vs `Pt(1,2)` — but the unlisted record's §8.3 generated
// `=` does, and both should answer the same; here the control answers true so
// the `?? false` coalesce has no effect and the program exits 0).
//
// BoxEscapeWalker's `isSynthesizedEqualsCallee` consult in `unresolvedOrSynthesizedEquals`
// already covers the `NullCondMemberExpr` arm (it was widened by #5068 for
// `Object`-argument ASan), so the lifetime exemption is in place — the
// remaining gap is purely the dispatch decision inside `lowerNullCondCall`
// (and its helper `lowerNullCondValueSemCall`, the nullable-value-receiver
// value-semantics call helper that the .`tryLowerValueSemanticsCall` path
// already routes to for the .`-form).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// T1 — THE ISSUE'S EXACT REPRO. Pre-fix this exits 1 with the verifier error
// (the AOT fork returns `ran=false`); post-fix the synthesized Equals
// witnesses field-by-field equality, the value-semantics dispatch lowers,
// and the program runs to completion.
BEZEL_TEST(codegen_nullcond_member_synthesized_equals_record_5096,
           issue_repro_nullcond_synthesized_equals_record_compiles_spec_9_1_8_3) {
    JitResult r = runAot(
        "module M; "
        "record Pt(X: int, Y: int): IEquatable<Pt>; "
        "class App "
        "  public static method Main(): int "
        "    var r := 0; "
        "    var np: Pt? := new Pt(1, 2); "
        "    if np?.Equals(new Pt(1, 2)) ?? false then r := r + 1; end if; "
        "    return r; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// T2 — LOOP-AMPLIFIED VARIANT. §7.5's receiver-null branch must NOT keep
// emitting a fresh block per iteration; the synthesized Equals must route
// through the SAME value-semantics core for every iteration.
BEZEL_TEST(codegen_nullcond_member_synthesized_equals_record_5096,
           loop_amplified_synthesized_equals_does_not_miscompile_spec_9_1_7_5) {
    JitResult r = runAot(
        "module M; "
        "record Pt(X: int, Y: int): IEquatable<Pt>; "
        "class App "
        "  public static method Main(): int "
        "    var r := 0; "
        "    var i := 0; "
        "    while i < 50 do "
        "      var np: Pt? := new Pt(1, 2); "
        "      if np?.Equals(new Pt(1, 2)) ?? false then r := r + 1; end if; "
        "      var nullp: Pt? := null; "
        "      if not (nullp?.Equals(new Pt(1, 2)) ?? false) then r := r + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    return r; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 50 non-null iterations + 50 null-receiver iterations = 100.
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// T3 — THE `Object`-ARGUMENT FORM. §8.1 / §8.6: the `Equals(Object?)` overload
// is what `Object` receivers bind to; for a record that lists
// `IEquatable<Self>`, the synthesized Equals must still answer correctly
// when the argument is a boxed record (§8.6 boxing). BoxEscapeWalker's
// `isSynthesizedEqualsCallee` consult is exercised here for the `?.` arm.
BEZEL_TEST(codegen_nullcond_member_synthesized_equals_record_5096,
           object_argument_boxed_equal_record_spec_8_1_8_6_9_1) {
    JitResult r = runAot(
        "module M; "
        "record Pt(X: int, Y: int): IEquatable<Pt>; "
        "class App "
        "  public static method Main(): int "
        "    var hits := 0; "
        "    var i := 0; "
        "    while i < 50 do "
        "      var np: Pt? := new Pt(1, 2); "
        "      var o: Object := new Pt(1, 2); "
        "      var o2: Object := new Pt(1, 3); "
        "      if np?.Equals(o) ?? false then hits := hits + 1; end if; "
        "      if not (np?.Equals(o2) ?? true) then hits := hits + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    return hits; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// T4 — CONTROL: the SAME program with an UNLISTED record. §9.1 makes the
// synthesized `Equals(T)` the record's value-semantics answer regardless of
// any explicit `IEquatable<Self>` listing, so the fix's change to the dispatch
// path must not break the unlisted case. Pre-fix the unlisted record also
// reaches `nullcall.end` and falls through into the verifier failure on r23-c28
// only for the LISTED case; this row's pre-fix status is "passes" because the
// abstract-ms path is never taken for an unlisted record — the fix must keep
// it that way.
BEZEL_TEST(codegen_nullcond_member_synthesized_equals_record_5096,
           control_unlisted_record_nullcond_equals_spec_7_5_9_1) {
    JitResult r = runAot(
        "module M; "
        "record Q(X: int, Y: int); "
        "class App "
        "  public static method Main(): int "
        "    var r := 0; "
        "    var nq: Q? := new Q(1, 2); "
        "    if nq?.Equals(new Q(1, 2)) ?? false then r := r + 1; end if; "
        "    return r; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// T5 — NEGATIVE CONTROL: the plain `.` (non-nullable) `Equals` call. This
// already compiles and runs correctly via #5068. The fix must not regress it:
// no `?.`, no null, just a record value receiver calling the synthesized
// Equals through the .`-form value-semantics dispatch.
BEZEL_TEST(codegen_nullcond_member_synthesized_equals_record_5096,
           negative_control_plain_dot_equals_on_listed_record_spec_9_1_8_3) {
    JitResult r = runAot(
        "module M; "
        "record Pt(X: int, Y: int): IEquatable<Pt>; "
        "class App "
        "  public static method Main(): int "
        "    var r := 0; "
        "    var p1 := new Pt(1, 2); "
        "    var p2 := new Pt(1, 2); "
        "    var p3 := new Pt(1, 3); "
        "    if p1.Equals(p2) then r := r + 1; end if; "
        "    if not p1.Equals(p3) then r := r + 2; end if; "
        "    return r; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}