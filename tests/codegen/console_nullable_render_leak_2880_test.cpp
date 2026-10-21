// Spec-derived regression test for issue #2880.
//
// `Console.Write` / `Console.WriteLine` on a NULLABLE-VALUE-TYPED argument
// (`int?`, `long?`, `double?`, `char?`, `decimal?`, a nullable enum / record /
// choice / value-builtin) never reclaimed the string it rendered.
//
// Spec (language spec revision v0.240; stdlib citations are marked as such):
//   §8.5 "`ToString`" — the per-kind rendering the console emits.
//   §18.5 "Interpolated strings" — the source of the `"null"` spelling this
//     branch emits for an absent value ("`null` renders as the four characters
//     `null`" is the rule the branch mirrors).
//   §28.9 "Runtime-managed types" — lists "`string` — pervasive; created by
//     literals, interpolation, concatenation, and library methods", and leaves
//     the mechanism open: "The implementation of runtime-managed types'
//     lifecycle (reference counting, region allocation, or other) is not part
//     of this specification." Bezel's implementation chose reference counting,
//     so a render the console mints is discharged by RELEASING it once.
//   §7.2 "Nullable value types" — the tagged representation this branch exists
//     for; `string?` is NOT one (it is a nullable reference), which is what
//     keeps the borrow shape below out of reach.
//   stdlib §9.6 "`String`" — "public pure override method ToString(): string;
//     // returns this" — the ONE render that is a borrow, and the reason the
//     over-release oracle exists.
//
// SYMPTOM, measured on this lane's base (which already carries #2875) with
// `bezelc --sanitize=address`, 20 iterations with runtime-built values and a
// churn allocation between the write and the re-read:
//
//     SUMMARY: AddressSanitizer: 4687 byte(s) leaked in 240 allocation(s)
//
// Six renders leaked once per iteration — `int?`, `long?`, `double?`, `char?`,
// `decimal?` and a nullable RECORD (its final concat). The `bool?`, nullable-
// enum and `null` arms leaked nothing, because those render to an immortal
// literal. After the fix the same program is CLEAN (0 leaked) and prints
// byte-identical output.
//
// THE CAUSE. `tryLowerConsoleCall`'s `isNullableValueTy` branch built its
// `strPtr` as a PHI over a `hasValue` / `null` split and then wrote it WITHOUT
// discharging it, on the reasoning recorded in the source: "Freeing a PHI
// result conditionally after the write requires a second hasValue check; the
// minor leak matches what the interpolation path accepts."
//
// THE FIX. That reasoning was true of the raw `free()` the code used before
// #2875 and is no longer true of `emitStringRelease` (`bz_string_release`),
// which is rc-aware and sentinel-safe. One unconditional release covers both
// arms — a real free on the minting arm, a no-op on the `"null"` literal and on
// a `bool?`/enum sentinel — so the branch now sets `releaseRender = true` and
// falls through to the function's SHARED write+discharge tail instead of doing
// its own write and returning early.
//
// ⚠ ORACLE DISCIPLINE. "No leak" is BLIND to the opposite defect, and the
// opposite defect is live in this exact code: #2875 was an OVER-release on the
// neighbouring branch that silently destroyed the caller's own string. Every
// case below therefore RE-READS each source value after the console call, with
// a runtime-built churn allocation in between so a freed block is likely
// reused, and the string cases print their contents back. Values are built at
// RUNTIME so nothing is constant-folded into an immortal literal (which would
// make a release a vacuous no-op).
//
// ⚠ WHY AN UNCONDITIONAL RELEASE IS EXACTLY-ONCE HERE. #2066 records that a
// value-typed field's `ToString` "is a fresh +1 producer", and that the ONE
// borrow is `string`'s identity render. `isNullableValueTy` admits only
// nullable primitive-value / enum / record / choice / value-builtin types, so
// `string?` (a nullable REFERENCE — a plain `ptr`) cannot reach this branch,
// and neither can a nullable class or interface. The callee-side unknown #2879
// records for an `Object`-returning method is therefore structurally out of
// reach here. The `string?` case below pins that.
//
// ⚠ NOT this fix's, and deliberately kept out of these programs: a record with
// a STRING field leaks that field once per construction (`new R("t" +
// i.ToString(), i)`), with or without any console call at all — that is #2866,
// reproduced here in isolation while probing. Every record below is int-only so
// the oracle observes this defect and not that one.

// #6778 (owner ruling, §7.1, §22.6, v0.340): a null-initialized `T?` has no
// applicable Write/WriteLine overload, so the absent values in (B) and (E)
// print through §18.5's interpolation hole, which renders the same `null`.
// The narrowed (initializer-bound) values keep reaching the console branch.
// The old spelling is a reject row in
// tests/codegen/console_nullable_arg_6778_test.cpp.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

std::string out(const JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// --- (A) The leaking grid: every MINTING nullable-value render. ------------
// Loop-amplified over 20 iterations, each operand rebuilt from the loop
// counter, with churn between the write and the re-read. Pre-fix this leaked
// 4687 bytes in 240 allocations; post-fix it is clean. Every source is read
// back after its write, so the fix cannot pass by over-releasing instead.
const char* kMintingRendersReleasedOnce =
    "module M2880A; "
    "public record Pt2880A(X: int, Y: int); "
    "public choice Sh2880A Dot, Circle(r: int); end Sh2880A; "
    "public class C2880A "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      k := k + 1; "
    "      var ni: int?        := k; "
    "      var nl: long?       := long(k) * 1000000000L; "
    "      var nd: double?     := double(k) + 0.5; "
    "      var nc: char?       := char(65 + (k mod 26)); "
    "      var nm: decimal?    := decimal(k) + 0.25m; "
    "      var nr: Pt2880A?    := new Pt2880A(k, k + 1); "
    "      var ns: Sh2880A?    := Sh2880A.Circle(k); "
    "      var nt: TimeSpan?   := TimeSpan.FromTicks(long(k) * 1000L); "
    "      Console.WriteLine(ni); "
    "      Console.WriteLine(nl); "
    "      Console.WriteLine(nd); "
    "      Console.Write(nc); "
    "      Console.WriteLine(); "
    "      Console.WriteLine(nm); "
    "      Console.WriteLine(nr); "
    "      Console.WriteLine(ns); "
    "      Console.WriteLine(nt); "
    "      var churn: string := \"c\" + k.ToString() + \"-\" + (k * 7).ToString(); "
    "      t := t + churn.Length - churn.Length; "
    // Sum over k = 1..20:  ni! = 210, nl!/1e9 = 210, the nd! check = 20,
    // int(nc!) = sum 66..85 = 1510, the nm! check = 20, nr!.X = 210,
    // the nt! check = 20  ->  2200.
    "      t := t + ni! + int(nl! / 1000000000L) + int(nc!) + nr!.X "
    "           + (if nd! = double(k) + 0.5 then 1 else 0) "
    "           + (if nm! = decimal(k) + 0.25m then 1 else 0) "
    "           + (if nt! = TimeSpan.FromTicks(long(k) * 1000L) then 1 else 0); "
    "    end while; "
    "    if t = 2200 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2880A;";

// --- (B) The SENTINEL arms: a release here must be a NO-OP, not a free. ----
// `bool?` renders stdlib §9.4's "True"/"False" globals; an enum with §8.5's
// default renders the type's name; an absent value renders §18.5's "null"
// literal. All three are immortal string LITERALS with the #422 rc sentinel.
// A raw free() on any of them aborts the process ("free(): invalid pointer" —
// the #2875 failure, one path over); `bz_string_release` no-ops. Nothing here
// allocates, so leak detection is ON and must stay clean.
const char* kSentinelAndNullArms =
    "module M2880B; "
    "public enum Color2880B Red, Green; end Color2880B; "
    "public record Pt2880B(X: int, Y: int); "
    "public class C2880B "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      k := k + 1; "
    "      var nb: bool?       := (k mod 2) = 0; "
    "      var ne: Color2880B? := (if (k mod 2) = 0 then Color2880B.Red "
    "                             else Color2880B.Green); "
    "      var ni: int?        := null; "
    "      var nn: bool?       := null; "
    "      var nd: double?     := null; "
    "      var nc: char?       := null; "
    "      var nm: decimal?    := null; "
    "      var nr: Pt2880B?    := null; "
    "      Console.WriteLine(nb); "
    "      Console.WriteLine(ne); "
    // #6778 (§7.1, §22.6, v0.340): a null-initialized `T?` is not narrowed,
    // so it has no applicable Write/WriteLine overload; the absent values
    // print through §18.5's hole, which renders the same `null`.
    "      Console.WriteLine($\"{ni}\"); "
    "      Console.Write($\"{nn}\"); "
    "      Console.WriteLine(); "
    "      Console.WriteLine($\"{nd}\"); "
    "      Console.WriteLine($\"{nc}\"); "
    "      Console.WriteLine($\"{nm}\"); "
    "      Console.WriteLine($\"{nr}\"); "
    "      t := t + int(ne!) + 1; "
    "      if ni <> null or nn <> null or nd <> null or nc <> null or "
    "         nm <> null or nr <> null then t := t + 1000; end; "
    "    end while; "
    "    if t = 30 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2880B;";

// --- (C) OVER-RELEASE ORACLE: the borrow shape must be untouched. ----------
// stdlib §9.6 — `String.ToString` "returns this", so a string render is a
// BORROW of the caller's own live string; releasing it would destroy the
// caller's data (exactly what #2875 was). §7.2 keeps `string?` out of this
// branch (it is a nullable REFERENCE, a plain `ptr`, not the tagged struct), so
// this asserts the branch's own admission rule, not just an accident. Both a
// live `string?` and a null one, re-read AND re-printed after 20 rounds.
const char* kNullableStringUntouched =
    "module M2880C; "
    "public class C2880C "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    var live: string? := \"ab\" + \"cd\".Substring(0, 2); "
    "    var gone: string? := null; "
    "    while k < 20 do "
    "      k := k + 1; "
    "      Console.WriteLine(live); "
    "      Console.Write(live); "
    "      Console.WriteLine(); "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + live!.Length - 3 + churn.Length - churn.Length; "
    "      if gone <> null then t := t + 1000; end; "
    "    end while; "
    "    Console.WriteLine(live); "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2880C;";

// --- (D) An enum whose USER `ToString` MINTS (§10.5). ----------------------
// The one shape on this branch that calls back into user code
// (`emitEnumToString` dispatches a user `ToString`). A minting override's
// result is a `+1` the console must release exactly once; an override that
// returns literals yields a sentinel on which the release is a no-op. Both are
// exercised, leak-asserted, with the source read back.
const char* kEnumUserToStringArms =
    "module M2880D; "
    "public enum Lit2880D "
    "  Hearts, Spades; "
    "  public method ToString(): string "
    "    return (if this = Lit2880D.Hearts then \"H\" else \"S\"); "
    "  end ToString; "
    "end Lit2880D; "
    "public enum Mint2880D "
    "  Ace, King; "
    "  public method ToString(): string "
    "    return \"R\" + (if this = Mint2880D.Ace then \"1\" else \"2\"); "
    "  end ToString; "
    "end Mint2880D; "
    "public class C2880D "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      k := k + 1; "
    "      var lt: Lit2880D?  := (if (k mod 2) = 0 then Lit2880D.Hearts "
    "                            else Lit2880D.Spades); "
    "      var mt: Mint2880D? := (if (k mod 2) = 0 then Mint2880D.Ace "
    "                            else Mint2880D.King); "
    "      Console.WriteLine(lt); "
    "      Console.WriteLine(mt); "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + int(lt!) + int(mt!) + 1 + churn.Length - churn.Length; "
    "    end while; "
    "    if t = 40 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2880D;";

// --- (E) VALUE CONTROL: the fix is refcount bookkeeping only. --------------
// Not one rendered character may change — §8.5's per-kind table through the
// nullable branch, both facets, plus §18.5's "null" spelling for the absent
// arm. Passes before and after by construction; it is here so a future change
// to the discharge cannot silently alter the output.
const char* kRenderedTextUnchanged =
    "module M2880E; "
    "public record Pt2880E(X: int, Y: int); "
    "public enum Color2880E Red, Green; end Color2880E; "
    "public choice Sh2880E Dot, Circle(r: int); end Sh2880E; "
    "public class C2880E "
    "  public static method Main(): int "
    "    var ni: int?        := 7; "
    "    var nl: long?       := 8L; "
    "    var nd: double?     := 1.5; "
    "    var nc: char?       := 'z'; "
    "    var nb: bool?       := true; "
    "    var nm: decimal?    := 2.5m; "
    "    var ne: Color2880E? := Color2880E.Green; "
    "    var nr: Pt2880E?    := new Pt2880E(1, 2); "
    "    var ns: Sh2880E?    := Sh2880E.Circle(3); "
    "    var nt: TimeSpan?   := TimeSpan.FromTicks(0L); "
    "    var nn: int?        := null; "
    "    var nz: bool?       := null; "
    "    Console.WriteLine(ni); "
    "    Console.WriteLine(nl); "
    "    Console.WriteLine(nd); "
    "    Console.WriteLine(nc); "
    "    Console.WriteLine(nb); "
    "    Console.WriteLine(nm); "
    "    Console.WriteLine(ne); "
    "    Console.WriteLine(nr); "
    "    Console.WriteLine(ns); "
    "    Console.WriteLine(nt); "
    // #6778 (§7.1, §22.6, v0.340): the two null-initialized values print
    // through §18.5's hole, which renders the same `null`.
    "    Console.WriteLine($\"{nn}\"); "
    "    Console.WriteLine($\"{nz}\"); "
    "    Console.Write(ni); "
    "    Console.WriteLine(); "
    "    return 41; "
    "  end Main; "
    "end C2880E;";

}  // namespace

// ===========================================================================
// (A) THE DEFECT: every minting nullable-value render is now released exactly
// once. Pre-fix: 4687 bytes leaked in 240 allocations (LSan forces exit 1, so
// `r.ran` is false). §28.9.
// ===========================================================================
BEZEL_TEST(codegen_console_nullable_render_leak_2880,
           minting_nullable_renders_released_exactly_once_28_9) {
    JitResult plain = runAot(kMintingRendersReleasedOnce);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kMintingRendersReleasedOnce);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: false (LSan: 240 allocations)
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // and the values are still correct
}

// ===========================================================================
// (B) The `bool?` / default-enum / `null` arms render immortal LITERALS; the
// discharge must be a no-op on them, never a free of a constant global (the
// #2875 abort, one branch over). §18.5 / §8.5.
// ===========================================================================
BEZEL_TEST(codegen_console_nullable_render_leak_2880,
           sentinel_and_null_arms_are_not_freed_18_5) {
    JitResult plain = runAot(kSentinelAndNullArms);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kSentinelAndNullArms);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (C) OVER-RELEASE guard: a `string?` argument is a nullable REFERENCE (§7.2),
// so it never reaches this branch and its identity render (stdlib §9.6
// "returns this") is never discharged here. Asserted by reading and reprinting
// the string after 20 rounds.
//
// ⚠ LENIENT (ASan on, leak detection OFF): a `string?` local built from a
// producer is the pre-existing #2846/#2866 residual family, not this fix's;
// the memory-safety half is what this case is for.
// ===========================================================================
BEZEL_TEST(codegen_console_nullable_render_leak_2880,
           nullable_string_argument_is_untouched_9_6) {
    JitResult plain = runAot(kNullableStringUntouched);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    std::string expected;
    for (int i = 0; i < 41; ++i) expected += "abcd\n";
    BEZEL_EXPECT_TRUE(out(plain) == expected);
    JitResult r = runAotCheckedLenient(kNullableStringUntouched);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (D) §10.5: a nullable enum with a USER `ToString` — the only arm of this
// branch that calls into user code. A minting override is released once; a
// literal-returning one is a no-op.
// ===========================================================================
BEZEL_TEST(codegen_console_nullable_render_leak_2880,
           enum_user_tostring_arms_released_once_10_5) {
    JitResult plain = runAot(kEnumUserToStringArms);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kEnumUserToStringArms);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: false (the minting arm leaked)
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (E) VALUE CONTROL (passes both ways by construction): the rendered text is
// byte-identical. §8.5's per-kind table plus §18.5's "null".
// ===========================================================================
BEZEL_TEST(codegen_console_nullable_render_leak_2880,
           rendered_text_unchanged_8_5) {
    JitResult plain = runAot(kRenderedTextUnchanged);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    BEZEL_EXPECT_TRUE(out(plain) ==
                      // #2934 (§8.5, v0.252): the ENUM row is now the
                      // module-path-qualified name ("Color2880E" ->
                      // "M2880E.Color2880E"); the record head and the
                      // §35.7 choice head stay unqualified.
                      "7\n8\n1.5\nz\nTrue\n2.5\nM2880E.Color2880E\n"
                      "Pt2880E(X: 1, Y: 2)\nSh2880E.Circle(r: 3)\n"
                      "00:00:00\nnull\nnull\n7\n");
}
