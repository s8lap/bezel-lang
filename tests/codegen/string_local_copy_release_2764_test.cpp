// Codegen / refcounting — issue #2764: copying a heap-produced `string` local
// into a SECOND local (`var other := built;`) leaked the string. One leaked
// heap string (2 allocations — the `bz_string` header plus its data buffer) per
// copy, with no interface, indexer or method call involved.
//
// Spec: §4.6 "`string`"; §28.9 "Runtime-managed types" — "`string` — pervasive;
// created by literals, interpolation, concatenation, and library methods", and
// "The implementation of runtime-managed types' lifecycle (reference counting,
// region allocation, or other) is not part of this specification"; §28.3
// "Local variables, ownership, and borrows", kernel K1.6 — "Values of
// runtime-managed type (`string`, boxed value types, function values) and of
// ordinary value type carry no classification and no obligation". Spec revision
// v0.240.
//
// So neither `built` nor `other` carries a §28 disposal obligation; what the
// two bindings do carry, in THIS implementation, is one reference each, and the
// observable requirement is that the heap string be reclaimed. It was not.
//
// THE DEFECT. `$"ov{n}"` is a `+1` producer, so `built` IS a scope-end-release
// candidate. It lost the release to the conservative escape walk:
// StringEscapeWalker (LlvmBackend_Setup.cpp) arms a producer local's release
// only if the same walk proves the local never escapes, and a bare
// `var other := built;` was a use of `built` that was not on the walk's
// safe-read list. `other` never had a release of its own to compensate: its
// initializer is an IdentifierExpr, i.e. a borrow. Neither binding released.
//
// WHY IT IS SAFE TO RELAX. #429 already gives EVERY borrow-initialized string
// local an INDEPENDENT +1: armStringLocalRefcount emits `bz_string_retain` into
// the new slot for every member of `stringBorrowAll_`, escaping or not.
// Confirmed mechanically on the pre-fix compiler by disassembling the issue's
// own repro: `bz$M.C$Main` contained exactly one `bz_string_retain` (the alias
// bind) paired with one `bz_string_release` (the alias's scope-end release),
// and `built`'s slot was never released. So the alias site ALREADY minted its
// own reference — the fix is only the walker half, and the two now move in
// lockstep because the walker consults the very set the retain is gated on.
// Same coordinated shape as #2201's `var t := s!` arm and #2717's index-argument
// arm: the alias keeps its own +1, so the source keeps its own release.
//
// ⚠ Every string here is built at RUNTIME (`$"ov-{i}"`): `"ab" + "cd"`
// constant-folds to an rc sentinel on which `bz_string_release` no-ops — the
// trap that has made probes in this family vacuous repeatedly. Case (E) is the
// deliberate literal case and is a value/no-crash guard only.
//
// ⚠ EVERY case is LOOP-AMPLIFIED (and each iteration allocates a churn string).
// This is load-bearing, not decoration: a ONE-copy `Main` is NOT a reliable
// LSan oracle. The issue's own single-copy repro was observed to report
// "36 byte(s) leaked in 2 allocation(s)" on some runs of the same binary and
// clean on others — a leftover pointer in a register or stack slot makes LSan
// treat the block as reachable. Twenty iterations put the leak beyond that
// noise: every case below was stable across repeated runs.
//
// MECHANICAL FAIL -> PASS on 1779228 (`--sanitize=address`, per case):
//   (A) 751 B / 40 allocs -> clean      (I) 371 B / 20 -> clean
//   (B) 371 B / 20        -> clean      (N) 751 B / 40 -> clean
//   (C) 751 B / 40        -> clean      (P) 371 B / 20 -> clean
//   (F) 371 B / 20        -> clean      (Q) 333 B / 18 -> clean
//   (G) 333 B / 18        -> clean      (H) 751 B / 40 -> clean
// The PLAIN (unsanitized) exit code is IDENTICAL before and after in every one
// of the 16 programs — a leak is invisible without a sanitizer, so LSan is the
// oracle that moves and the plain assertions pin that the fix froze nothing
// early.
//
// ⚠ DELIBERATE EXCLUSIONS — cases (J), (M) (case (L), the `string?` target,
// left this list under #5245: `string?` is now admitted to `stringBorrowAll_`
// and asserted clean). The relaxation fires only for
// a copy target that armStringLocalRefcount actually RETAINS (membership in
// `stringBorrowAll_`). A `using var` target is
// excluded from the borrow-candidate set by construction, and an alias STORED
// into a class field escapes on its own account. Each keeps the pre-existing
// bounded leak, which is the safe direction — relaxing for them would free the
// source while an un-retained alias still named it, a use-after-free. Each is
// pinned below as a characterization guard that asserts the PAIR: a leak IS
// reported and specifically NOT a use-after-free / double-free / SEGV.
//
// fix #3416 (§21.1 / §28.9, v0.283): case (K), the `const` copy target, LEFT
// this list and is now a RELAXED case, asserted clean. It was excluded here on
// a true premise — "a LocalConstDecl is never retained" — that #3416 changed:
// §21.1's runtime `const` is now admitted to `stringBorrowAll_` by
// `collectStringReleaseEligible`, so armStringLocalRefcount retains it at bind
// exactly as it retains a `var`, and StringEscapeWalker's `LocalConstDecl` arm
// carries the same relaxation its `LocalDecl` sibling does. The condition the
// relaxation rests on therefore holds for a `const`, and it holds MORE strongly:
// §21.1 makes a `const` "not an assignable location", so the alias's own
// reference cannot be displaced before its scope-end release.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The concatenated diagnostic text of a checked run. runAotChecked funnels the
// program's whole sanitizer report into one Error diagnostic, so a test can tell
// WHICH sanitizer fired — a leak is a very different verdict from a double free,
// and `r.ran` is false for both.
std::string sanitizerText(const JitResult& r) {
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    return all;
}

// A checked run that is expected to LEAK (a documented exclusion) and, in
// particular, NOT to have freed anything early.
void expectLeakButNoMemoryError(const JitResult& r) {
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") != std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("SEGV") == std::string::npos);
}

// sum over i in 1..20 of len("xx-{i}") = 9*4 + 11*5 = 91.
constexpr int kLenSum20 = 91;
// sum over i in 1..10 of len("xx-{i}") = 9*4 + 1*5 = 41.
constexpr int kLenSum10 = 41;

// The churn allocation every case makes between the copy and the reads. The
// `< 0` test is never true; it exists to force the read so the local cannot be
// elided, and the allocate/free pair means a prematurely freed block cannot be
// read back intact on the plain (unsanitized) path.
#define CHURN                                                                  \
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "                  \
    "      if churn.Length < 0 then return 99; end; "

// =====================================================================
// (A) THE ISSUE'S OWN REPRO, loop-amplified — a bare `var other := built;`
// copy of an interpolation producer. §4.6 / §28.9.
// =====================================================================
const char* kCopyIntoSecondLocal =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var built := $\"ov-{i}\"; "
    "      var other := built; "
    CHURN
    "      total := total + other.Length; "
    "    end; "
    "    return total; "                    // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (B) COPY-OF-A-COPY. Each link in the chain mints its own +1, so all three
// slots must release. All three are read, so an over-release anywhere in the
// chain is a use-after-free rather than a silent miscount. §28.3 K1.6.
// =====================================================================
const char* kCopyOfACopy =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      var a := $\"aa-{i}\"; "
    "      var b := a; "
    "      var c := b; "
    CHURN
    "      total := total + a.Length + b.Length + c.Length; "
    "    end; "
    "    return total; "                    // 3 * 41 = 123
    "  end Main; "
    "end C;";

// =====================================================================
// (C) The copy lives in a STRICTLY NARROWER scope than its source, and the
// source is read AFTER that scope (and after the alias's scope-end release)
// with a churn allocation in between. This is the shape that would expose the
// relaxation as a use-after-free if the alias did not own its own reference.
// §28.3.
// =====================================================================
const char* kCopyInInnerScopeSourceReadAfter =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var built := $\"in-{i}\"; "
    "      if i > 0 then "
    "        var inner := built; "
    "        total := total + inner.Length; "
    "      end; "
    CHURN
    "      total := total + built.Length; "  // read after the inner release
    "    end; "
    "    return total; "                    // 2 * 91 = 182
    "  end Main; "
    "end C;";

// =====================================================================
// (D) OVER-CORRECTION GUARD — a copy of a `string` PARAMETER. §28.1 makes a
// `string` parameter an unmodified borrow whose one reference belongs to the
// CALLER, so the callee's `var t := s;` must mint its OWN reference and must
// NOT arm anything that would release the caller's. Both `t` and `s` are read
// inside the callee and the caller reads its string again afterwards. This
// case is CLEAN both before and after the fix — it is a guard, not an oracle.
// =====================================================================
const char* kCopyOfStringParameter =
    "module M; class C "
    "  private static method Take(s: string): int "
    "    var t := s; "
    "    return t.Length + s.Length; "
    "  end Take; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      var built := $\"pp-{i}\"; "
    "      var r := C.Take(built); "
    CHURN
    "      total := total + r + built.Length; "
    "    end; "
    "    return total; "                    // 3 * 41 = 123
    "  end Main; "
    "end C;";

// =====================================================================
// (E) A copy of a LITERAL. The literal is an rc sentinel on which retain and
// release are no-ops, so no special case is needed and none was added — this
// pins that the new arm did not disturb it. CLEAN both before and after.
// §4.6 / §28.9.
// =====================================================================
const char* kCopyOfLiteral =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var lit := \"hello\"; "
    "      var other := lit; "
    CHURN
    "      total := total + other.Length + lit.Length; "
    "    end; "
    "    return total; "                    // 20 * 10 = 200
    "  end Main; "
    "end C;";

// =====================================================================
// (F) The SOURCE is REASSIGNED after the copy. #1997's plain-`:=` relaxation
// keeps the source's slot at exactly +1 on its FINAL value
// (lowerStringLocalReassign releases the displaced one), and the alias holds an
// independent reference to the ORIGINAL — so both the displaced value and the
// final one must be reclaimed, and the alias must survive the displacement.
// Both are read. §4.6 / §21.1.
// =====================================================================
const char* kSourceReassignedAfterCopy =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      var built := $\"re-{i}\"; "
    "      var other := built; "
    "      built := $\"zz-{i}\"; "
    CHURN
    "      total := total + other.Length + built.Length; "
    "    end; "
    "    return total; "                    // 2 * 41 = 82
    "  end Main; "
    "end C;";

// =====================================================================
// (G) The ALIAS is CAPTURED BY A LAMBDA. §28.9's capture rule makes the env
// cell an un-retained alias of the captured local, which is why the alias's own
// scope-end release is suppressed and the closure's env destructor takes it
// over. The SOURCE is not captured, so it keeps its release — and the two must
// not collide. §27.5 / §28.9.
// =====================================================================
const char* kAliasCapturedByLambda =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      var built := $\"cp-{i}\"; "
    "      var other := built; "
    "      var f := lambda() => other.Length; "
    CHURN
    "      total := total + f() + built.Length; "
    "    end; "
    "    return total; "                    // 2 * 41 = 82
    "  end Main; "
    "end C;";

// =====================================================================
// (H) The ALIAS is RETURNED. The alias escapes, so it is retained but not
// released (the pre-existing safe direction); the SOURCE, which the return does
// not name, becomes releasable. The caller reads the returned value with churn
// in between, so an over-release inside the callee is a use-after-free here.
// §28.3 / §22.
// =====================================================================
const char* kAliasReturnedFromCallee =
    "module M; class C "
    "  private static method Make(i: int): string "
    "    var built := $\"rt-{i}\"; "
    "    var other := built; "
    "    return other; "
    "  end Make; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var got := C.Make(i); "
    CHURN
    "      total := total + got.Length; "
    "    end; "
    "    return total; "                    // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (I) The ALIAS is passed as a CALL ARGUMENT (#1430's call-bounded borrow) and
// the SOURCE is read after the call. §28.3.
// =====================================================================
const char* kAliasPassedToCall =
    "module M; class C "
    "  private static method Sink(s: string): int "
    "    return s.Length; "
    "  end Sink; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      var built := $\"ec-{i}\"; "
    "      var other := built; "
    "      var r := C.Sink(other); "
    CHURN
    "      total := total + r + built.Length; "
    "    end; "
    "    return total; "                    // 2 * 41 = 82
    "  end Main; "
    "end C;";

// =====================================================================
// (N) The copy is declared inside `case` ARM BODIES — the walk recurses into
// arm statements, so both arms' copies must be recognized. §19.1 / §19.2.
// =====================================================================
const char* kCopyInsideCaseArms =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var built := $\"ca-{i}\"; "
    "      case i mod 2 of "
    "        0 => do "
    "          var even := built; "
    "          total := total + even.Length; "
    "        end, "
    "        _ => do "
    "          var odd := built; "
    "          total := total + odd.Length; "
    "        end "
    "      end; "
    CHURN
    "    end; "
    "    return total; "                    // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (P) Both aliases feed a CONCAT (`other + built`) — a `+1` producer whose
// operands are borrow-read. All three strings must be reclaimed. §4.6.
// =====================================================================
const char* kConcatOfSourceAndAlias =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      var built := $\"cc-{i}\"; "
    "      var other := built; "
    "      var joined := other + built; "
    CHURN
    "      total := total + joined.Length; "
    "    end; "
    "    return total; "                    // 2 * 41 = 82
    "  end Main; "
    "end C;";

// =====================================================================
// (Q) A three-link chain in which the MIDDLE link escapes (it is a call
// argument) while the first and last do not. The escaping link keeps its
// retained reference (safe); the other two must still release exactly once,
// and both are read afterwards. §28.3.
// =====================================================================
const char* kAliasChainWithEscapingMiddle =
    "module M; class C "
    "  private static method Sink(s: string): int "
    "    return s.Length; "
    "  end Sink; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      var a := $\"ch-{i}\"; "
    "      var b := a; "
    "      var c := b; "
    "      var r := C.Sink(b); "
    CHURN
    "      total := total + a.Length + c.Length + r; "
    "    end; "
    "    return total; "                    // 3 * 41 = 123
    "  end Main; "
    "end C;";

// =====================================================================
// (J), (L), (M) THE DELIBERATE EXCLUSIONS. Each still leaks; none may have freed
// anything early.
// =====================================================================

// (J) the alias is STORED INTO A CLASS FIELD. The field store retains
// (lowerStringFieldStore, §4.6 / §28.9), so the field owns its own reference —
// but the alias itself escapes and so keeps its retained reference un-released,
// exactly as before this fix. The caller reads the field back with churn in
// between.
const char* kAliasStoredIntoClassField =
    "module M; class Holder "
    "  public Last: string := \"\"; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var built := $\"fs-{i}\"; "
    "      var other := built; "
    "      h.Last := other; "
    CHURN
    "      total := total + h.Last.Length; "
    "    end; "
    "    h.Last := \"\"; "
    "    return total; "                    // 91
    "  end Main; "
    "end C;";

// (K) a `const` copy target. Was a deliberate exclusion (a LocalConstDecl was
// not collected and never retained, so the source kept tainting and leaked);
// fix #3416 admits §21.1's runtime `const` to `stringBorrowAll_` and gives the
// walker's LocalConstDecl arm the same borrow relaxation, so this is now a
// RELAXED case and the program is clean. 20 iterations leaked pre-#3416.
const char* kConstCopyTarget =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var built := $\"co-{i}\"; "
    "      const other := built; "
    CHURN
    "      total := total + other.Length; "
    "    end; "
    "    return total; "                    // 91
    "  end Main; "
    "end C;";

// (L) a `string?`-declared copy target — admitted to the borrow-candidate set
// since #5245 (it used to be excluded: non-nullable-only), so it is retained at
// bind and released at scope end.
const char* kNullableCopyTarget =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var built := $\"nu-{i}\"; "
    "      var other: string? := built; "
    CHURN
    "      total := total + other!.Length; "
    "    end; "
    "    return total; "                    // 91
    "  end Main; "
    "end C;";

// (M) a `using var` copy target — excluded by the `!ld->isUsing` gate.
const char* kUsingVarCopyTarget =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var built := $\"uv-{i}\"; "
    "      using var other := built; "
    CHURN
    "      total := total + other.Length; "
    "    end; "
    "    return total; "                    // 91
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------
// (A) the issue's own repro.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           copy_into_second_local_is_released_spec_28_9) {
    JitResult r = runAotChecked(kCopyIntoSecondLocal);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 751 B / 40 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           copy_into_second_local_plain_path_value_invariant_spec_28_9) {
    // NOT a #2764 oracle — a leak is invisible without a sanitizer, so this
    // passes with and without the fix. It pins that the fix did not change the
    // computed value, i.e. that nothing is freed early.
    JitResult r = runAot(kCopyIntoSecondLocal);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (B) copy-of-a-copy.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           copy_of_a_copy_releases_every_link_spec_28_3) {
    JitResult r = runAotChecked(kCopyOfACopy);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 371 B / 20 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 3 * kLenSum10);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           copy_of_a_copy_plain_path_value_invariant_spec_28_3) {
    JitResult r = runAot(kCopyOfACopy);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3 * kLenSum10);
}

// ---------------------------------------------------------------------
// (C) the copy is in a narrower scope; the source is read afterwards.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           copy_in_inner_scope_source_survives_spec_28_3) {
    JitResult r = runAotChecked(kCopyInInnerScopeSourceReadAfter);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 751 B / 40 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum20);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           copy_in_inner_scope_plain_path_value_invariant_spec_28_3) {
    // The strong direction here: were the alias's release to free the source's
    // only reference, `built.Length` after the inner scope reads freed memory.
    JitResult r = runAot(kCopyInInnerScopeSourceReadAfter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum20);
}

// ---------------------------------------------------------------------
// (D) / (E) OVER-CORRECTION GUARDS — clean before AND after.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           copy_of_string_parameter_stays_balanced_spec_28_1) {
    // A `string` parameter's one reference belongs to the caller (§28.1); the
    // callee's copy must mint its own. Clean on both sides of the fix — a guard
    // against widening the relaxation to a location that is never retained.
    JitResult r = runAotChecked(kCopyOfStringParameter);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3 * kLenSum10);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           copy_of_literal_stays_balanced_spec_4_6) {
    // The rc sentinel: retain and release are no-ops, so no special case was
    // needed. Clean on both sides of the fix.
    JitResult r = runAotChecked(kCopyOfLiteral);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// ---------------------------------------------------------------------
// (F) the source is reassigned after the copy.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           source_reassigned_after_copy_releases_both_values_spec_4_6) {
    JitResult r = runAotChecked(kSourceReassignedAfterCopy);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 371 B / 20 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum10);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           source_reassigned_after_copy_plain_path_value_invariant_spec_4_6) {
    // The alias must survive the source's reassignment: the displaced value is
    // released by lowerStringLocalReassign, and `other.Length` reads it after.
    JitResult r = runAot(kSourceReassignedAfterCopy);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum10);
}

// ---------------------------------------------------------------------
// (G) the alias is captured by a lambda.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           alias_captured_by_lambda_source_still_released_spec_28_9) {
    JitResult r = runAotChecked(kAliasCapturedByLambda);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 333 B / 18 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum10);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           alias_captured_by_lambda_plain_path_value_invariant_spec_28_9) {
    JitResult r = runAot(kAliasCapturedByLambda);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum10);
}

// ---------------------------------------------------------------------
// (H) the alias is returned.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           alias_returned_from_callee_source_released_spec_28_3) {
    JitResult r = runAotChecked(kAliasReturnedFromCallee);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 751 B / 40 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           alias_returned_from_callee_plain_path_value_invariant_spec_28_3) {
    // The returned value is read in the caller with churn in between, so an
    // over-release inside the callee shows up here as a wrong length.
    JitResult r = runAot(kAliasReturnedFromCallee);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (I) the alias is a call argument.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           alias_passed_as_call_argument_spec_28_3) {
    JitResult r = runAotChecked(kAliasPassedToCall);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 371 B / 20 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum10);
}

// ---------------------------------------------------------------------
// (N) the copy is inside `case` arm bodies.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           copy_inside_case_arm_bodies_spec_19_2) {
    JitResult r = runAotChecked(kCopyInsideCaseArms);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 751 B / 40 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (P) both aliases feed a concat.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           concat_of_source_and_alias_spec_4_6) {
    JitResult r = runAotChecked(kConcatOfSourceAndAlias);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 371 B / 20 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum10);
}

// ---------------------------------------------------------------------
// (Q) a chain whose middle link escapes.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           alias_chain_with_escaping_middle_link_spec_28_3) {
    JitResult r = runAotChecked(kAliasChainWithEscapingMiddle);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 333 B / 18 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 3 * kLenSum10);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           alias_chain_plain_path_value_invariant_spec_28_3) {
    JitResult r = runAot(kAliasChainWithEscapingMiddle);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3 * kLenSum10);
}

// ---------------------------------------------------------------------
// (J), (L), (M) the deliberate exclusions, pinned as characterization guards.
// (K), the `const` copy target, LEFT this group in #3416 — see its own note
// below and the header. Each remaining one
// still LEAKS (the safe direction) and must not report a memory error: relaxing
// for any of them would free the source while an un-retained alias named it.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_local_copy_release_2764,
           alias_stored_into_class_field_still_leaks_spec_28_9) {
    JitResult r = runAotChecked(kAliasStoredIntoClassField);
    if (wasSkipped(r)) return;
    expectLeakButNoMemoryError(r);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           alias_stored_into_class_field_value_is_correct_spec_28_9) {
    JitResult r = runAot(kAliasStoredIntoClassField);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// fix #3416 (§21.1 / §28.9, v0.283): case (K) moved from the exclusion list to
// the relaxed set — see the note in this file's header. §21.1's runtime `const`
// copy target is now retained at bind and released at scope end like its `var`
// twin, so the SOURCE's producer +1 is released too and the program is clean.
// The assertion is inverted deliberately, in the same change that made it true.
BEZEL_TEST(codegen_string_local_copy_release_2764,
           const_copy_target_is_relaxed_like_var_spec_21_1) {
    JitResult r = runAotChecked(kConstCopyTarget);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // leaked 20 producers pre-#3416
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           const_copy_target_value_is_correct_spec_21_1) {
    JitResult r = runAot(kConstCopyTarget);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// fix #5245 (§4.6 / §28.9, v0.313): case (L) LEFT the exclusion list. A
// `string?` borrow target is now admitted to `stringBorrowAll_` (retained at
// bind, released at scope end), so this program is clean; it used to report the
// bounded leak this row pinned (and a `string?` borrow of a field dangled).
BEZEL_TEST(codegen_string_local_copy_release_2764,
           nullable_copy_target_is_released_spec_28_9) {
    JitResult r = runAotChecked(kNullableCopyTarget);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           nullable_copy_target_value_is_correct_spec_7) {
    JitResult r = runAot(kNullableCopyTarget);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           using_var_copy_target_is_deliberately_excluded_spec_28_10) {
    JitResult r = runAotChecked(kUsingVarCopyTarget);
    if (wasSkipped(r)) return;
    expectLeakButNoMemoryError(r);
}

BEZEL_TEST(codegen_string_local_copy_release_2764,
           using_var_copy_target_value_is_correct_spec_28_10) {
    JitResult r = runAot(kUsingVarCopyTarget);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}
