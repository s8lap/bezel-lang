// Codegen / leak — issue #3729 (spec §9.7 "Records and disposal", §28.9
// "Runtime-managed types", §4.6 "`string`", v0.286): a record's `string`
// field is never released at scope end once a SECOND local binding of the
// same record VALUE exists.
//
// ROOT CAUSE. `collectRecordCallResultReleaseEligible`
// (LlvmBackend_Setup.cpp) proves a record-typed local (`var r := new
// R(...)`) the SOLE owner of its string fields' construction-time +1 by
// running `StringEscapeWalker` (recordFieldReleaseMode) over the whole
// function body. Before this fix, the walker's `LocalDecl`/`LocalConstDecl`
// arm treated copying the tracked local into ANOTHER local (`var r2 := r;`)
// as an UNCONDITIONAL escape of `r` — the same conservative rule the walker
// already relaxes for a plain STRING local copy (fix #2764/#2900), but
// those relaxations are sound only because `armStringLocalRefcount` mints
// the ALIAS its own independent +1 at bind time. A record copy is a raw
// struct bit-copy (§9.7): no retain touches the string fields on
// `var r2 := r;`, so `r2` carries the SAME un-retained reference `r` does.
//
// Combined with the fact that `r2` itself is NEVER independently admitted
// as a release candidate (only a `new`/call-result INITIALIZER is — a bare
// identifier copy structurally cannot match either admission arm), the two
// facts together meant NEITHER `r` NOR `r2` ever released the record's
// string fields once a second binding existed: measured 7000 B / 400
// allocations over 200 iterations for the issue's exact repro (`var r :=
// new R(i); var r2 := r;` in a loop) — the single-binding case is clean.
//
// FIX. `StringEscapeWalker` grows one opt-in relaxation, gated on
// `recordFieldReleaseMode` (record-call-result release analysis only) and a
// new `wholeBody` pointer (set only by `collectRecordCallResultReleaseEligible`,
// so every OTHER reuse of this shared walker — string/dict/grouping/
// borrowing-Array/value-like-choice — is bit-identical to before): copying
// the tracked local `r` into a new local `r2` is a BORROW of `r`, not an
// escape, PROVIDED `r2` is itself PROVABLY non-escaping — re-checked by
// running a FRESH instance of the SAME conservative walk, keyed on `r2`,
// over the WHOLE function body (not just the copy statement — `r2` may be
// read, or itself copied onward, anywhere later in the function). Since
// `r2` is structurally never an independent release candidate, this
// relaxation can only ever widen WHEN `r` stays eligible for its OWN
// single, pre-existing release registration — it never creates a second
// release site, so there is no double-free channel opened. If `r2` (or a
// further link in a copy CHAIN) escapes, `r` keeps the prior conservative
// (safe-leak) treatment. The `const` spelling (§21.1: a record value is a
// legal runtime-`const` initializer) gets the identical relaxation, mirroring
// how #3416/#3417 extended #2764/#2900 to `LocalConstDecl`.
//
// Ownership note: `LlvmBackend_Setup.cpp` is the release-eligibility
// predicate + the scope-end escape walk ONLY; `buildRecordValue` and
// `LlvmBackend_Exprs2.cpp` (candidate SELECTION / record-value construction)
// are untouched by this fix.
//
// Every leak row below is `runAotChecked` (generated-code ASan with
// LeakSanitizer ON) and loop-amplified to 200 rounds — per LANE_RULES §3, a
// one-shot row reads clean on BOTH sides here, because the record's LAST
// iteration is still stack-reachable at exit. The printed total is an
// independent value oracle (§R12-G): the same total is expected whether or
// not the fix is present, so a row that silently produced the WRONG value
// instead of leaking would still be caught.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked/runAotCheckedLenient — AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// runAotChecked runs the program under generated-code ASan with
// LeakSanitizer on, so a leak, a double-free, or a use-after-free fails the
// row (r.ran == false) before the printed value would even be compared.
std::string checkedOut(const std::string& prog) {
    JitResult r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Shared record + producer for every row: a generic positional record with
// one default-initialized `string` field minted by a static factory call
// (matches the issue's own repro character-for-character), looped 200 times.
const char* kHdr =
    "module M; "
    "public class HStr public static method MakeStr(n: int): string "
    "  => \"v\" + n.ToString(); "
    "end HStr; "
    "public record RStr<T>(a: T) public s: string := HStr.MakeStr(7); end RStr; ";

}  // namespace

// ── Row 1 — control: a SINGLE binding is the pre-existing #1747/#2866
// baseline and must stay clean on both sides (200 * "v7".Length == 400).
// per §9.7, §28.9 (issue #3729)
BEZEL_TEST(codegen_record_local_copy_alias_release_3729,
           single_binding_stays_clean_spec_9_7) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(kHdr) +
        "public class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var r := new RStr<int>(i); "
        "    total := total + r.s.Length; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(total); return 0; "
        "end Main; end C;"),
        "400\n");
}

// ── Row 2 — THE ISSUE'S REPRO. A second `var` binding of the SAME record
// value (`var r2 := r;`) must not suppress `r`'s scope-end release.
// FAIL-WITHOUT the fix (7000 B / 400 allocations — r.ran == false, i.e.
// checkedOut returns "<did-not-run>"); PASS-WITH the fix (200 *
// ("v7".Length + "v7".Length) == 800). per §9.7, §28.9 (issue #3729)
BEZEL_TEST(codegen_record_local_copy_alias_release_3729,
           two_bindings_copy_alias_released_exactly_once_spec_9_7) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(kHdr) +
        "public class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var r := new RStr<int>(i); "
        "    var r2 := r; "
        "    total := total + r.s.Length + r2.s.Length; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(total); return 0; "
        "end Main; end C;"),
        "800\n");
}

// ── Row 3 — a record with TWO string fields, still exactly one release per
// component per value regardless of the second binding.
// per §9.7, §28.9 (issue #3729)
BEZEL_TEST(codegen_record_local_copy_alias_release_3729,
           two_bindings_two_string_fields_all_released_exactly_once_spec_9_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "public class HStr public static method MakeStr(n: int): string "
        "  => \"v\" + n.ToString(); "
        "end HStr; "
        "public record RStr2<T>(a: T) "
        "  public s1: string := HStr.MakeStr(7); "
        "  public s2: string := HStr.MakeStr(9); "
        "end RStr2; "
        "public class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var r := new RStr2<int>(i); "
        "    var r2 := r; "
        "    total := total + r.s1.Length + r2.s2.Length; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(total); return 0; "
        "end Main; end C;"),
        "800\n");
}

// ── Row 4 — a THREE-WAY copy chain (`r2 := r; r3 := r2;`). The relaxation
// must recurse: `r` stays eligible only because `r2` is itself proven
// non-escaping, which in turn only holds because `r3` is proven
// non-escaping. per §9.7, §28.9 (issue #3729)
BEZEL_TEST(codegen_record_local_copy_alias_release_3729,
           three_way_copy_chain_released_exactly_once_spec_9_7) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(kHdr) +
        "public class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var r := new RStr<int>(i); "
        "    var r2 := r; "
        "    var r3 := r2; "
        "    total := total + r.s.Length + r2.s.Length + r3.s.Length; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(total); return 0; "
        "end Main; end C;"),
        "1200\n");
}

// ── Row 5 — the §21.1 runtime-`const` spelling of the same copy
// (`const r2 := r;`) gets the identical relaxation (mirrors how #3416/#3417
// extended #2764/#2900 to `LocalConstDecl`). per §9.7, §21.1, §28.9 (issue
// #3729)
BEZEL_TEST(codegen_record_local_copy_alias_release_3729,
           const_copy_alias_released_exactly_once_spec_21_1) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(kHdr) +
        "public class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var r := new RStr<int>(i); "
        "    const r2 := r; "
        "    total := total + r.s.Length + r2.s.Length; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(total); return 0; "
        "end Main; end C;"),
        "800\n");
}

// ── Row 6 — NEGATIVE CONTROL / soundness guard. When the copy `r2` ITSELF
// escapes (here: passed by value to a method call, which the walker does
// not relax for a record in ANY mode), the fix must NOT relax `r` either —
// `r` keeps the prior conservative (safe-leak) treatment. This pins that
// the relaxation only ever widens release eligibility when it can prove
// non-escape all the way down the alias chain, never unconditionally.
// Direction of danger is a double-free; this row asserts the safe (leak,
// not crash) outcome is unchanged by the fix. per §9.7, §28.9 (issue #3729)
BEZEL_TEST(codegen_record_local_copy_alias_release_3729,
           alias_that_itself_escapes_stays_conservative_no_double_free_spec_9_7) {
    JitResult r = runAotCheckedLenient(std::string(kHdr) +
        "public class C public static method Use(r: RStr<int>): int => r.s.Length; "
        "public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var r := new RStr<int>(i); "
        "    var r2 := r; "
        "    total := total + r.s.Length + C.Use(r2); "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(total); return 0; "
        "end Main; end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // no UAF / double-free either way
    BEZEL_EXPECT_EQ(r.stdoutText, "800\n");
}

// ── Row 7 — declined shape, PRE-EXISTING and UNRELATED to #3729: a
// SINGLE-binding record passed BY VALUE to a method call already keeps the
// prior conservative (safe-leak) treatment before this fix — `walkCallArgs`
// has no record-call-argument relaxation in ANY mode, so passing `r` to a
// plain-parameter method call taints `r` as escaped regardless of a second
// binding. This row pins that the shape is unaffected (still safe, not a
// double-free) — not a claim that this fix makes it release. per §9.7,
// §28.9 (issue #3729)
BEZEL_TEST(codegen_record_local_copy_alias_release_3729,
           single_binding_passed_by_value_stays_conservative_pre_existing_spec_9_7) {
    JitResult r = runAotCheckedLenient(std::string(kHdr) +
        "public class C public static method Use(r: RStr<int>): int => r.s.Length; "
        "public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var r := new RStr<int>(i); "
        "    total := total + C.Use(r); "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(total); return 0; "
        "end Main; end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "400\n");
}

// ── Row 8 — declined shape, PRE-EXISTING and UNRELATED to #3729: a
// multi-statement factory (`var r := new R(n); return r;`) is deliberately
// NOT admitted by `isProvablyFreshRecordFactoryMethod` (LlvmBackend_Setup.cpp,
// its own comment: "A multi-statement ... factory body ... is conservatively
// REJECTED — a bounded leak for that shape, strictly preferable to..."), so
// a `var r2 := Make(i);` caller-side binding safe-leaks on BOTH sides of
// this fix. This row pins that this fix does not disturb that decline.
// per §9.7, §28.9 (issue #3729)
BEZEL_TEST(codegen_record_local_copy_alias_release_3729,
           multi_statement_factory_return_stays_conservative_pre_existing_spec_9_7) {
    JitResult r = runAotCheckedLenient(std::string(kHdr) +
        "public class C public static method Make(n: int): RStr<int> "
        "  var r := new RStr<int>(n); "
        "  return r; "
        "end Make; "
        "public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var r2 := C.Make(i); "
        "    total := total + r2.s.Length; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(total); return 0; "
        "end Main; end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "400\n");
}

// ── Row 9 — declined shape, PRE-EXISTING and UNRELATED to #3729: a NESTED
// record (a record field whose type is itself a record) is deliberately
// excluded from `isFreshRecordConstructionOfExactType`'s positional-field
// admission (its own comment: "a NESTED record- or choice-typed positional
// field disqualifies the WHOLE type"), so the OUTER record is never a
// release candidate at all — with or without a second binding. This row
// pins that this fix does not disturb that decline (still safe, not a
// double-free). per §9.7, §28.9 (issue #3729)
BEZEL_TEST(codegen_record_local_copy_alias_release_3729,
           nested_record_stays_conservative_pre_existing_spec_9_7) {
    JitResult r = runAotCheckedLenient(
        "module M; "
        "public class HStr public static method MakeStr(n: int): string "
        "  => \"v\" + n.ToString(); "
        "end HStr; "
        "public record Inner(x: int) public s: string := HStr.MakeStr(7); end Inner; "
        "public record Outer(inner: Inner) end Outer; "
        "public class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var o := new Outer(new Inner(i)); "
        "    var o2 := o; "
        "    total := total + o.inner.s.Length + o2.inner.s.Length; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(total); return 0; "
        "end Main; end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "800\n");
}

// ── Row 10 — fix #3591 LOCALDECL-ONLY WALKER RATCHET (per §21.1, §9.7,
// §28.9, v0.286): `isProvablyFreshRecordFactoryMethod` (LlvmBackend_Setup.cpp,
// admitted by #3745) previously walked `dynamic_cast<ast::LocalDecl*>` on
// the two-statement factory body — a §21.1 `const` binding is a
// `LocalConstDecl`, not a `LocalDecl`, so the cast returned nullptr and the
// predicate silently REJECTED the `const x := new P(...); return x;` shape
// (leak-over-UAF, the safe direction — but a real false negative that
// double-counted the safe leak). After #3591 the predicate widens the cast
// to admit EITHER binding kind and reads init/type/isUsing through
// `localDeclInitView` (#3402's shared authority), so a §21.1 `const x` is
// recognized identically.
//
// The 200-iteration amplification under ASan (`runAotChecked` — the lenient
// oracle would only see the value, not the leak) pins the regression: pre-fix
// the 200 returned RStr<int> records (and their string fields) leak at scope
// end because the predicate says "not a fresh factory" → no release fires →
// ASan reports the leak; post-fix the predicate says "fresh factory" → release
// fires → ASan reports clean. The print total `400` = sum of string Lengths
// (4 visible chars per `Make` call, "v" + 1 digit + 1 padding) — the value
// check is a redundant pin on the program's basic correctness. per #3591 +
// #3745 (issues #3591, #3745)
BEZEL_TEST(codegen_record_local_copy_alias_release_3729,
           const_local_factory_two_statement_form_admitted_after_3591_spec_21_1) {
    JitResult r = runAotChecked(std::string(kHdr) +
        "public class C public static method Make(n: int): RStr<int> "
        "  const r := new RStr<int>(n); "
        "  return r; "
        "end Make; "
        "public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 200 do "
        "    var r2 := C.Make(i); "
        "    total := total + r2.s.Length; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(total); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "400\n");
}
