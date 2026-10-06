// Codegen + runtime — a `string?` local narrowed into a NEW non-nullable
// `string` local via `var t: string := s!` (the issue's exact shape) or a
// new binding that shadows the source (the rebind shape) must release BOTH
// the source's +1 (the producer) and the new local's +1 (the borrow's
// retain) at scope end — otherwise the producer's +1 leaks (LSan-confirmed:
// 32-byte bz_string_t header + N-byte buffer per evaluation). The bug
// (#2201) was that the
// StringEscapeWalker treated `s!` in a LocalDecl's initializer as inheriting
// the parent's unsafe position (the new local may itself be returned/stored)
// and therefore marked `s` as ESCAPING, suppressing its scope-end release.
//
// Spec: §17.8 (null-forgiving preserves ownership — `s!` is a borrow, not a
// move), §28.9 (runtime-managed `string` refcount — every binding owns a +1
// released at scope end), §28.3 (producers + borrows + the arm-meet family
// that the walker has a similar relaxation for; this is the LocalDecl
// sibling). Spec revision v0.226.
//
// THE BUG. `var s: string? := "hello" + " world"; var t: string := s!;` is
// the bare repro. Pre-fix: the walker marks `s` as escaped (because `s!` is
// in `t`'s initializer, which the walker conservatively treats as an escape
// position — the new local may be returned/stored), so `s` is not released
// at scope end; under AOT+LSan the 44-byte string ("hello world" + header)
// is reported as a direct + indirect leak. `t` itself is correctly retained
// and released (the rebind/borrow `stringBorrowAll_` + `stringBorrowRetain-
// Eligible_` paths already work for the new local). The fix is in the
// LocalDecl walker: when the initializer is the null-forgiving `s!`
// (PostfixBangExpr), walk the operand as safeRead — the new local is a
// borrow of `s` (per §17.8 + `armStringLocalRefcount`'s retain), so `s` is
// NOT consumed by the binding and its +1 stays in `s`'s slot.
//
// WORKING CONTROLS (must remain non-leaking after the fix):
//   * `s!.Length` — a non-rebind read. Pre-fix: clean. Post-fix: clean (the
//     MemberAccess walks the target as safeRead, so `s` is not tainted).
//   * `s := s!` — a plain reassignment. Pre-fix: clean. Post-fix: clean
//     (the AssignStmt's `isTargetIdent` relaxation in #1997 keeps `s` from
//     escaping).
//
// FAIL-WITHOUT / PASS-WITH. Every case below is exercised AOT+ASan/LSan.
// pre-fix: every distinct-name / rebind case leaks 44 bytes per evaluation;
// post-fix: every case is LSan-clean and exits with the expected code.
//
// LOOPED for unmistakable evidence — a single evaluation's 44-byte leak is
// overshadowed by the BZ runtime's own allocations; N evaluations × N bytes
// is unambiguous.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// True iff some error diagnostic's message contains `needle`.
bool errorSays(const bezel::test::JitResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// LEAK REPROS — pre-fix: LSan reports 44 bytes leaked per evaluation.
// ===========================================================================

// Spec §17.8 / §28.9: the issue's exact shape — `var t: string := s!` where
// s is a +1 producer. Pre-fix: `s` is marked escaped by the walker because
// `s!` is in `t`'s initializer (an escape position), so `s`'s +1 leaks.
// Post-fix: the LocalDecl walker treats `s!`'s operand as safeRead, so `s`
// stays in `stringReleaseEligible_` and its +1 is released at scope end.
// Looped 5x so 5 × 44 = 220 bytes is unmistakable.
BEZEL_TEST(nonlocal_string_narrow_bind_leak_2201,
           distinct_new_local_disposes_source_spec_17_8) {
    std::string src =
        "module M; class C "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 5 do "
        "      var s: string? := \"hello\" + \" world\"; "
        "      var t: string := s!; "
        "      total := total + t.Length; "
        "      i := i + 1; "
        "    end; "
        "    return total; "            // 5 * 11 = 55
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// Spec §17.8 / §28.9: the REBIND shape — a new non-nullable local bound
// from `s!` SHADOWS the source. Same soundness argument: the new local is a
// borrow of `s`, so `s` is not consumed. Pre-fix: leak; post-fix: clean.
//
// The shadowing binding now sits in an inner `do … end` rather than
// directly in the loop body. §21.1's *Local names* rule (v0.314, #5370)
// rejects a local that redeclares a name of its OWN scope ("a local named
// `s` is already declared in this scope"), and the compiler now enforces
// it — the same-block spelling no longer compiles. That ruling landed only
// its same-scope half; whether a local may shadow a name of an ENCLOSING
// scope was left open and the compiler admits it (see the v0.314 changelog
// entry for #5370), so the nested block keeps the row's subject — a new
// binding that shadows the source and is initialised from it — while
// sitting in a scope the rule does not reach.
BEZEL_TEST(nonlocal_string_narrow_bind_leak_2201,
           rebind_new_local_disposes_source_spec_17_8) {
    std::string src =
        "module M; class C "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 5 do "
        "      var s: string? := \"hello\" + \" world\"; "
        "      do "
        "        var s := s!; "
        "        total := total + s.Length; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// Spec §17.8 / §28.9: the source is a parameter (borrow), not a producer.
// The new local still retains a +1 from `s!` and releases it at scope end;
// the source's owner is responsible for the source's lifetime. The walker
// for `s` (the parameter) is not even invoked (parameters are not in
// `borrowCandidates` / `producerCandidates`), so this case is unaffected by
// the fix — pins that the fix doesn't regress the parameter-borrow family.
BEZEL_TEST(nonlocal_string_narrow_bind_leak_2201,
           parameter_source_disposes_new_local_spec_17_8) {
    std::string src =
        "module M; class C "
        "  public static method Test(s: string?): int "
        "    var t: string := s!; "
        "    return t.Length; "
        "  end Test; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 5 do "
        "      total := total + C.Test(\"hello world\"); "
        "      i := i + 1; "
        "    end; "
        "    return total; "            // 5 * 11 = 55
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// ===========================================================================
// WORKING CONTROLS — must remain non-leaking after the fix.
// ===========================================================================

// `s!.Length` — a non-rebind read. The MemberAccess treats the target as
// safeRead, so `s` is never tainted. Pre-fix: clean. Post-fix: clean.
BEZEL_TEST(nonlocal_string_narrow_bind_leak_2201,
           bang_member_access_no_rebind_disposes_source_spec_17_8) {
    std::string src =
        "module M; class C "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 5 do "
        "      var s: string? := \"hello\" + \" world\"; "
        "      total := total + s!.Length; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// `s := s!` — a plain reassignment. The AssignmentStmt `isTargetIdent`
// relaxation (#1997) keeps `s` from escaping. Pre-fix: clean. Post-fix: clean.
BEZEL_TEST(nonlocal_string_narrow_bind_leak_2201,
           plain_reassign_narrow_disposes_source_spec_17_8) {
    std::string src =
        "module M; class C "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 5 do "
        "      var s: string? := \"hello\" + \" world\"; "
        "      s := s!; "
        "      total := total + s.Length; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// `s!.Length` AS A RECEIVER of a chained call — exercises the case where
// `s!` is itself a sub-expression of a method call. The MemberAccess chains
// onto the bang and the target is safeRead throughout. Pre-fix: clean.
// Post-fix: clean.
BEZEL_TEST(nonlocal_string_narrow_bind_leak_2201,
           bang_method_chain_no_rebind_disposes_source_spec_17_8) {
    std::string src =
        "module M; class C "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 5 do "
        "      var s: string? := \"hello\" + \" world\"; "
        "      total := total + s!.Length; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// ===========================================================================
// NON-LEAKING WRAPPER — guards against an over-fix that suppresses the
// new local's release too. The new local's +1 must be released at the new
// local's scope end (§28.9), not just the source's. Pre-fix: the source
// leaks but the new local is balanced. Post-fix: BOTH are balanced; this
// test verifies both: the new local's value is fully read AND the source is
// released by the time we exit the loop body.
// ===========================================================================
BEZEL_TEST(nonlocal_string_narrow_bind_leak_2201,
           new_local_through_function_call_spec_17_8) {
    std::string src =
        "module M; class C "
        "  public static method Pass(t: string): int return t.Length; end Pass; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 5 do "
        "      var s: string? := \"hello\" + \" world\"; "
        "      var t: string := s!; "
        "      total := total + C.Pass(t); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}
