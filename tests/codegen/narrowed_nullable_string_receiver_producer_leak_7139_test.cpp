// Codegen / leak — a §7.4-NARROWED `string?` receiver made EVERY
// string-producing §9.6 instance method leak its producer's reference.
// Issue #7139.
//
// Spec: language spec §7.4 (null narrowing — "Member access, method calls,
// and indexing on a nullable value are compile errors unless the compiler has
// narrowed it to non-null"), §7.11 ("The rule reads the operand's static
// type, so an operand that §7.4 has narrowed non-null is a non-nullable
// operand and the ordinary operator applies to it unchanged"), §9.6 (the
// `string` class), §28.9 (runtime-managed types). Spec revision v0.343.
//
// THE BUG. `Emitter::isPlusOneStringProducer` (src/codegen/LlvmBackend_Setup.cpp)
// decides whether a `string`-typed expression hands its consumer a fresh owned
// reference (+1) or a borrow (+0). Its instance arm keyed on the receiver's
// DECLARED type:
//
//     auto* tt = ctx_.exprType(*ma->target);
//     if (tt && tt->kind == sema::TypeRef::Kind::Primitive &&
//         tt->primitive == sema::PrimitiveType::String &&
//         !tt->nullable) {            // <-- THE DEFECT
//
// ⇒ `ctx_.exprType` reports the DECLARED type, and for `var s: string?` that
// is `string?` — so the arm was skipped and the fresh `bz_string` the
// lowering minted was classified as a BORROW. The consumer then RETAINED on
// top of the producer's +1 and the scope-end release dropped it back to 1;
// the producer's original +1 never died, so the 32-byte `bz_string` header
// and its data buffer leaked. MEASURED before the fix: 83 byte(s) in 2
// allocation(s) per iteration — 3269 byte(s) in 79 allocation(s) for the
// 40-iteration loop of row (A) below.
//
// THE PROGRAM IS LEGAL — IT IS NOT A SEMANTIC ERROR. §7.4's
// narrowing-establishment bullet: "from a declaration whose initializer is
// such an `expr` — `var x: T? := expr;` … — which is the declaration's
// assignment" ⇒ `var s: string? := "x";` narrows `s` to non-null at that
// program point, with no `if` guard and no `!`. §7.11 then makes the
// narrowed receiver a NON-NULLABLE operand, so the ordinary (non-nullable)
// path is the one that runs. The §7.4 sema gate is CORRECT and is left
// untouched: the UN-narrowed forms (a `string?` local initialized with `null`,
// a `string?` PARAMETER) must stay REJECTED — rows (K) and (L) below pin
// that.
//
// THE FIX is in the recognizer, at the one shared guard (not in eleven call
// sites): the receiver's DECLARED nullability is not part of the producer
// contract. Sema's ONE §7.4 gate (ExprTC::resolveMemberAccess) rejects every
// nullable receiver that is not narrowed, so a member access whose receiver
// is still `string?` at this point IS a receiver §7.4 has narrowed non-null.
// The lowering agrees: tryLowerStringMethod does not branch on the
// receiver's nullability (`string?` is a BARE nullable pointer —
// isNullableRefTy) and mints the fresh string at rc := 1 either way. The
// recognizer and the lowering therefore stay in lockstep, and the consumer
// TRANSFERS the single reference instead of retaining on top of it.
//
// THE VALUES ARE RIGHT BOTH BEFORE AND AFTER THE FIX — only the leak was
// wrong. Every row below therefore pins the VALUE (the rendered length, and
// in row (E) the byte-exact stdout) as well as leak-freedom; a fix that
// changed a program's value would fail these rows.
//
// OBSERVABILITY. runAotChecked compiles, links and RUNS the generated code
// under ASan/LSan; a leaked string makes r.ran false and r.exitCode -1. Every
// case renders in a LOOP (40 iterations) and counts byte-correct results: the
// loop makes all but the final allocation unreachable, so LSan RELIABLY
// reports the leak pre-fix (a single bound-then-read string can linger in a
// register and evade LSan's reachability scan — a false negative that would
// make a one-shot test flaky). The returned count == the iteration count IFF
// every render is byte-correct AND the run is leak-clean, so the exit code
// pins the VALUE and r.ran pins leak-freedom. Each case FAILS without the
// fix (LSan reports the leaked strings -> r.ran false) and PASSES with it.
//
// Rows (A), (C) and (E) used the INSTANCE `Repeat(count)` until fix #7706
// (stdlib §9.6, v0.350: `Repeat` is declared only as a static member, so the
// instance spelling is rejected); they now use the declared instance
// `PadLeft` / `PadRight`, another §9.6 member that mints a +1 string.
//
// NOT IN SCOPE. The `?.` spelling (`s?.PadLeft(50)`) is a DIFFERENT AST node
// (ast::NullCondMemberExpr) and its own codegen gap is tracked separately
// (#7130); it is not folded in here.

#include <string>
#include <utility>
#include <vector>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A "render N times, count byte-correct results" program. `decl` binds the
// receiver(s); `render` is the expression under test; `expectedLen` is the
// exact length the render must have. Returns 40 iff every render matched AND
// the run is leak-clean.
std::string loopCount(const char* decl, const char* render, int expectedLen) {
    return std::string(
               "module M; class C "
               "  public static method Main(): int ") +
           decl +
           "    var count := 0; "
           "    var i := 0; "
           "    while i < 40 do "
           "      var t := " + render + "; "
           "      if t.Length = " + std::to_string(expectedLen) +
               " then count := count + 1; end; "
           "      i := i + 1; "
           "    end; "
           "    return count; "
           "  end Main; "
           "end C;";
}

// The same shape for SEVERAL members in one program: every listed render must
// have the listed length, or the iteration does not count. The count stays
// <= 40 (one increment per iteration) so the 8-bit exit status is never
// truncated.
std::string loopMany(const char* decl,
                     const std::vector<std::pair<std::string, int>>& checks) {
    std::string src = "module M; class C "
                       "  public static method Main(): int ";
    src += decl;
    src += "    var count := 0; "
           "    var i := 0; "
           "    while i < 40 do "
           "      var ok := true; ";
    for (const auto& c : checks) {
        src += "      if (" + c.first + ").Length <> " +
               std::to_string(c.second) + " then ok := false; end; ";
    }
    src +=
        "      if ok then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;";
    return src;
}

// The §7.4 `if x <> null` narrowing form — the SAME defect reached by a
// different narrowing (the issue measured 3320 B in 80 allocations for it).
std::string loopIfGuarded(const char* decl, const char* render, int expectedLen) {
    return std::string(
               "module M; class C "
               "  public static method Main(): int ") +
           decl +
           "    var count := 0; "
           "    var i := 0; "
           "    while i < 40 do "
           "      if s <> null then "
           "        var t := " + render + "; "
           "        if t.Length = " + std::to_string(expectedLen) +
               " then count := count + 1; end; "
           "      end; "
           "      i := i + 1; "
           "    end; "
           "    return count; "
           "  end Main; "
           "end C;";
}

}  // namespace

// =====================================================================
// (A) THE #7139 REPRO, exactly: `var s: string? := "x"; var t: string :=
// s.PadLeft(50);` — narrowed by the DECLARATION'S INITIALIZER (§7.4).
// Pre-fix: 83 B in 2 allocations per iteration (3269 B in 79 allocations
// over the 40-iteration loop).
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           declaration_initializer_narrowing_repeat_lsan_clean_spec_7_4) {
    JitResult r = runAotChecked(loopCount(
        "var s: string? := \"x\"; ",
        "s.PadLeft(50)", 50));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean: the producer's +1 released
    BEZEL_EXPECT_EQ(r.exitCode, 40); // 40 == every render byte-correct
}

// =====================================================================
// (B) The same narrowing, a DIFFERENT §9.6 member — the defect is the
// RECEIVER, not the member (stdlib §9.6 / §28.9).
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           declaration_initializer_narrowing_trim_lsan_clean_spec_7_4) {
    JitResult r = runAotChecked(loopCount(
        "var s: string? := \"x\"; ",
        "s.Trim()", 1));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (C) The §7.4 `if s <> null then` narrowing form — the SAME defect
// reached by a DIFFERENT narrowing. Pre-fix: 3320 B in 80 allocations.
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           if_guard_narrowing_repeat_lsan_clean_spec_7_4) {
    JitResult r = runAotChecked(loopIfGuarded(
        "var s: string? := \"x\"; ",
        "s.PadLeft(50)", 50));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (D) The whole §9.6 string-producing family on a §7.4-narrowed `string?`
// receiver — the fix belongs in the ONE shared guard, so every member
// must be clean (stdlib §9.6 / §28.9).
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           member_family_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopMany(
        "var s: string? := \"a,b,c\"; ",
        {{"s.ToUpper()", 5},
         {"s.ToLower()", 5},
         {"s.Trim()", 5},
         {"s.TrimStart()", 5},
         {"s.TrimEnd()", 5},
         {"s.Substring(0,3)", 3},
         {"s.Replace('a','z')", 5},
         {"s.PadLeft(6)", 6},
         {"s.Insert(0,\"z\")", 6},
         {"s.Remove(0,1)", 4},
         {"s.GetField(',',1)", 1}}));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (E) THE VALUE, byte-exact. The values are RIGHT both before and after
// the fix — only the leak was wrong — so the rendered string itself is
// pinned here, not just its length.
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           rendered_value_pinned_stdlib_9_6) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string? := \"x\"; "
        "    var t: string := s.PadRight(3, 'x'); "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(t.Length); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("xxx\n3\n"));
}

// =====================================================================
// (F) CONTROL — a NON-NULLABLE receiver. It was clean before the fix and
// must stay clean: the widened guard must not have introduced a
// double-release.
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           control_non_nullable_receiver_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var s: string := \"x\"; ",
        "s.Trim()", 1));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (G) CONTROL — the force-unwrap spelling `s!` (§7.8). The `!` yields a
// non-nullable operand, so this receiver was never `string?` at the
// recognizer; it must stay clean.
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           control_force_unwrap_receiver_lsan_clean_spec_7_8) {
    JitResult r = runAotChecked(loopCount(
        "var s: string? := \"x\"; ",
        "s!.Trim()", 1));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (H) CONTROL — the `??` spelling (§7.7). The `??` yields a non-nullable
// operand; it must stay clean.
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           control_coalesced_receiver_lsan_clean_spec_7_7) {
    JitResult r = runAotChecked(loopCount(
        "var s: string? := \"x\"; ",
        "(s ?? \"y\").Trim()", 1));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (I) CONTROL — a copy into a NON-NULLABLE local, then the member on it.
// The copy is the ordinary §7.4 remedy for a nullable field; it must stay
// clean.
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           control_copy_to_non_nullable_local_lsan_clean_spec_7_4) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string? := \"x\"; "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var u: string := s; "
        "      var t: string := u.Trim(); "
        "      if t.Length = 1 then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (J) CONTROL — an `int`-result member on the SAME narrowed receiver.
// `CountOf` mints no `string`, so it is not a producer at all; the widened
// guard must not have made it one (stdlib §9.6).
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           control_int_result_member_lsan_clean_stdlib_9_6) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string? := \"x\"; "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var t: int := s.CountOf(\"x\"); "
        "      if t = 1 then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (K) CONTROL — the `+` concat and the interpolation producer on the SAME
// narrowed receiver. Both were clean before the fix (§28.3's producers) and
// must stay clean — the widened guard must not have made them transfer a
// reference they never minted.
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           control_concat_and_interpolation_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(loopMany(
        "var s: string? := \"x\"; ",
        {{"s + \"y\"", 2}, {"$\"{s}\"", 1}}));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (L) REJECTION CONTROL — an UN-narrowed `string?` local (its initializer
// is `null`, so §7.4 establishes no narrowing). §7.4's gate must keep
// rejecting it: the fix is in the recognizer and must NOT weaken the
// sema gate into accepting a program the spec forbids.
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           rejection_control_un_narrowed_local_still_rejected_spec_7_4) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string? := null; "
        "    var t: string := s.Trim(); "
        "    Console.WriteLine(t.Length); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "requires narrowing it to non-null first"));
}

// =====================================================================
// (M) REJECTION CONTROL — a `string?` PARAMETER as the receiver. A
// parameter is never narrowed by an initializer, so §7.4's gate must keep
// rejecting it.
// =====================================================================
BEZEL_TEST(narrowed_nullable_string_receiver_producer_leak_7139,
           rejection_control_nullable_parameter_still_rejected_spec_7_4) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method P(s: string?): int "
        "    var t: string := s.Trim(); "
        "    return t.Length; "
        "  end P; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "requires narrowing it to non-null first"));
}
