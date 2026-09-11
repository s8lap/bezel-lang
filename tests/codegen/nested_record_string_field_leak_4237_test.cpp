// fix #4237 (§9.7 "Records and disposal" / §28.9 "Runtime-managed types",
// v0.298): a LOCAL VARIABLE whose static type is a record whose
// string-owning field lies TWO OR MORE record layers deep never got its
// scope-end disposal registered — a safe-direction leak (LSan only; no
// UAF, no double-free), reproducible with no generics involved.
//
// Root cause: `Emitter::isFreshRecordConstructionOfExactType`
// (LlvmBackend_Setup.cpp) is the eligibility PROOF that gates whether a
// `var o := new Outer(...)` local is registered for scope-end
// `emitRecordStringFieldReleases` at all. Its per-field loop
// unconditionally disqualified the WHOLE construction whenever any
// positional field's type was Record OR Choice-kinded — it reasoned about
// DIRECT string fields only. This is a DIFFERENT function from
// `isRecordWithStringFieldsTypeRef` / `recordHasStringFields`, which
// already correctly walks nested records to DETECT a transitively-owned
// string (confirmed by fix #4232's own regression suite) and which the
// runtime release walk (`emitRecordStringFieldReleases_impl`,
// LlvmBackend_impl.hpp) already correctly RELEASES once reached. So the
// gap was purely in the eligibility proof deciding whether to even
// attempt registering the OUTER local — not in detection or in emission.
//
// THE FIX: a NESTED RECORD-typed field is now admitted when its own
// constructor argument is ITSELF provably a fresh, alias-free construction
// of exactly that nested record type — proven by
// `isFreshRecordConstructionOfExactType` calling ITSELF on the argument
// expression against the field's record declaration. A NESTED CHOICE-typed
// field is UNCHANGED (still disqualifies the whole type — a choice's
// active-case payload is not statically known here).
//
// MEASURED (this file's own FAIL-WITHOUT/PASS-WITH cycle, run by the fixing
// lane, `scripts/agent_check.sh --probe --asan`, 300 loop iterations):
//   * 2-layer repro (Inner/Outer, the issue's own program):
//       pre-fix:  SUMMARY: AddressSanitizer: 11290 byte(s) leaked in 600
//                 allocation(s). (9600 direct / 300 objects + 1690 indirect
//                 / 300 objects — matches the issue's own figures exactly)
//       post-fix: clean exit (acc mod 100 == 90), no LSan report.
//   * generic Box<T> pair (record Box<T>(Value: T); record Pair(A: Box<int>,
//     B: Box<string>)):
//       pre-fix:  leaked (B's string field never registered)
//       post-fix: clean exit (acc mod 100 == 90), no LSan report.
//   * 4-level "wide diamond" (Leaf->Mid[3 fields]->Node[2]->Top[2], ALL
//     inline nested `new` expressions, no intermediate locals):
//       pre-fix:  leaked
//       post-fix: clean exit (acc mod 100 == 90), no LSan report.
// See the fixing lane's report for the exact pre-fix byte/allocation counts
// on the generic-pair and wide-diamond variants (measured identically via
// the same probe, logs under /var/tmp/bezel-scratch/r18/4237-l10/logs/).
//
// SEND-BACK FB-4237 (review NEEDS-FIX round, see commit body): the original
// accept-side controls (rows 1 and 2, below) used `didNotCrash` alone,
// which passes identically under a wrongly-broadened admission that makes
// row 2's leak vanish instead of crashing -- not a control (§25). Rows 1
// and 2 now assert `leaksAtLeast` (a measured, margined leak-magnitude
// floor, immune to LeakSanitizer's own conservative-scan jitter -- see
// that helper's comment) and a factory-method-path row was added to record
// an independently-discovered, only-partially-explained discrepancy
// between the direct-local and call-result admission paths (issue #4258).
// See each row's own comment for what was measured and what remains open.

#include <regex>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
// LEAK oracle: runAotChecked sets sanitizeAddress on the GENERATED program
// and treats ANY sanitizer finding (including LSan's own exit-time leak
// report) as ran=false. A program that leaks therefore reports -999 here;
// a clean program reports its real exit code.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
// CRASH-FREEDOM oracle only (detect_leaks=0): proves no use-after-free /
// double-free / OOB, while tolerating an EXPECTED, still-safe leak. Used
// ONLY for the Choice-field control below, where the correct post-fix
// behavior is "still declines eligibility, still safely leaks" and the
// leak's MAGNITUDE is not this row's concern (the recursion never touches
// the Choice arm at all — see that row's own comment).
bool didNotCrash(const std::string& program) {
    auto r = runAotCheckedLenient(program);
    return r.ran;
}
// send-back FB-4237 (review of fix #4237, §25 "a control that passes under
// the wrong fix too is not a control"): `didNotCrash` alone cannot tell a
// correct decline from a WRONGLY BROADENED admission of this eligibility
// gate, because broadening it does not crash every affected shape -- for
// the two rows below it makes the expected, safe leak silently VANISH
// instead. Measured directly: a hand-built "shallow, non-recursive" variant
// of the fix (admits a nested Record field on a bare
// `dynamic_cast<ast::NewExpr>` + type match against the field's record
// declaration, WITHOUT recursing into that nested construction's own
// arguments -- i.e. exactly reverting the `if (!isFreshRecordConstruction
// OfExactType(arg.value.get(), fty->declaration)) return false;` guard this
// fix adds to a shallow type check) leaves row 1 UNCHANGED (its top-level
// argument is an IdentifierExpr, not a NewExpr, so even the shallow check
// declines it -- same as before) but makes row 2's leak disappear
// ENTIRELY: clean exit, 0 B, `didNotCrash` true on BOTH the real fix and
// the wrong variant, no discriminating power at all.
//
// This oracle asserts the LEAK IS STILL PRESENT, at or above a threshold --
// a wrongly broadened admission that makes the leak vanish now FAILS the
// row instead of passing it vacuously.
//
// The exact byte/allocation count is deliberately NOT pinned.
// LeakSanitizer's conservative stack scan is measurably NONDETERMINISTIC on
// these exact programs: repeated runs of the IDENTICAL compiled binary
// alternated between "8248 B / 398 allocation(s)" and "8290 B / 400
// allocation(s)" for the SAME 200-iteration program (off by exactly one
// loop iteration's worth, in both directions, across separate process
// invocations with no source or compiler difference -- a known LSan
// conservative-scan characteristic, not compiler nondeterminism; an exact-
// text assertion here would be a flaky test). `minBytes` is set comfortably
// below the smaller of the two observed figures, so the assertion is
// stable across that jitter while still failing hard against a leak that
// has vanished to (at most) a few bytes of unrelated harness noise.
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
    if (r.ran) return false;  // no sanitizer finding at all -- wrongly clean
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str()) >= minBytes;
    }
    return false;  // sanitizer fired but not with a leak SUMMARY (e.g. a
                    // crash/UAF instead) -- correctly fails this row too
}
}  // namespace

// ===========================================================================
// Leak-fixed rows: loop-amplified (300x, per LANE policy — a one-shot LSan
// row can pass on both sides), each row's Main returns 0 unconditionally so
// runChecked's oracle is exactly "did ASan/LSan report anything at all".
// ===========================================================================

// The issue's own repro, verbatim (module M; record Inner(S: string); record
// Outer(I: Inner); ...), loop-amplified. Depth 2: Outer.I is Inner,
// Inner.S is the owned string.
BEZEL_TEST(nested_record_string_field_leak_4237,
           two_layer_nested_record_string_field_no_leak_spec_9_7) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Inner(S: string); record Outer(I: Inner); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 300 do "
        "    var o := new Outer(new Inner(\"hi\" + i.ToString())); "
        "    var n := o.I.S.Length; "
        "    i += 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// The issue's generic variant: record Box<T>(Value: T); record Pair(A:
// Box<int>, B: Box<string>). B's field is a generic nested record whose
// bare type parameter is instantiated at `string` — proves the recursion
// composes correctly with #3964's bare-type-parameter resolution (the
// recursive call resolves `Value: T` through THIS construction's own
// `Box<string>` genericArgs, not the unbound declaration).
BEZEL_TEST(nested_record_string_field_leak_4237,
           generic_box_pair_nested_record_no_leak_spec_9_7_14_1) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Box<T>(Value: T); "
        "record Pair(A: Box<int>, B: Box<string>); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 300 do "
        "    var p := new Pair(new Box<int>(i), "
        "                      new Box<string>(\"s\" + i.ToString())); "
        "    var n := p.B.Value.Length; "
        "    i += 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// 4-level "wide diamond", entirely non-generic, ALL nested `new`
// expressions written inline (no intermediate `var` bindings feeding
// multiple fields) — Mid has THREE Leaf fields, Node and Top each have TWO
// fields of the next level down. Proves the fix handles a record with
// MULTIPLE record-typed fields per level (a fix that only recognized a
// single-field chain would still disqualify Mid/Node/Top here, since each
// has more than one record-typed positional field).
BEZEL_TEST(nested_record_string_field_leak_4237,
           wide_diamond_multi_field_four_level_no_leak_spec_9_7) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Leaf(S: string); "
        "record Mid(A: Leaf, B: Leaf, C: Leaf); "
        "record Node(A: Mid, B: Mid); "
        "record Top(A: Node, B: Node); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 300 do "
        "    var top := new Top("
        "      new Node("
        "        new Mid(new Leaf(\"a\" + i.ToString()), "
        "                new Leaf(\"b\" + i.ToString()), "
        "                new Leaf(\"c\" + i.ToString())), "
        "        new Mid(new Leaf(\"d\" + i.ToString()), "
        "                new Leaf(\"e\" + i.ToString()), "
        "                new Leaf(\"f\" + i.ToString()))), "
        "      new Node("
        "        new Mid(new Leaf(\"g\" + i.ToString()), "
        "                new Leaf(\"h\" + i.ToString()), "
        "                new Leaf(\"i\" + i.ToString())), "
        "        new Mid(new Leaf(\"j\" + i.ToString()), "
        "                new Leaf(\"k\" + i.ToString()), "
        "                new Leaf(\"l\" + i.ToString())))); "
        "    var n := top.A.A.A.S.Length; "
        "    i += 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ===========================================================================
// Value-correctness control (plain oracle, non-loop): proves the fix does
// not disturb what value is actually read back through the nested chain —
// a self-checking `if` assertion, matching the project's own
// struct-layout-correctness idiom (see #3945's
// struct_layout_matches_at_release_no_corruption).
// ===========================================================================
BEZEL_TEST(nested_record_string_field_leak_4237,
           two_layer_value_correctness_plain_oracle) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Inner(S: string); record Outer(I: Inner); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(\"nested\" + \"value\")); "
        "  if o.I.S <> \"nestedvalue\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ===========================================================================
// Accept-side / soundness controls (§15, §25): the recursion must NOT
// wrongly admit a nested field whose argument is a BORROW rather than a
// fresh construction, at ANY level — that would double-release or
// use-after-free a string another binding still owns. Rows 1 and 2 are
// EXPECTED to still leak safely, identically on both sides of the fix (the
// gate declines eligibility exactly as it did pre-fix, for a different,
// still-correct reason at the top level in row 1, and via the SAME
// recursive string-literal-or-+1-producer check at the nested level in row
// 2).
//
// send-back FB-4237: `didNotCrash` alone is NOT a control here — it also
// passes under a WRONGLY BROADENED admission that makes the leak vanish
// instead of crashing (measured; see `leaksAtLeast`'s own comment above for
// the exact wrong-variant construction and figures). Rows 1 and 2 therefore
// assert `leaksAtLeast` (still-leaking, at or above a measured-and-margined
// threshold), which fails when admission wrongly broadens the leak away —
// exactly the regression direction that matters for a fix that WIDENS
// eligibility. The Choice-field control below is unaffected by this fix
// (the recursion never touches the Choice arm — see its own comment) and
// stays on `didNotCrash`, where the magnitude is not in question.
// ===========================================================================

// Row 1: the WHOLE nested-record field is a borrow — an existing `Inner`
// local passed by identifier, not a fresh `new Inner(...)`. Pre-fix: the
// unconditional Record-field disqualification already rejects `outer`
// (never even inspects the argument). Post-fix: the recursive call's own
// top-of-function `dynamic_cast<ast::NewExpr>` fails on the identifier
// read, so it ALSO rejects `outer` — identical behavior, no regression.
// MEASURED (this lane's own rebuild, `scripts/dev_heavy.sh probe` +
// `--sanitize=address`, commit 83848a4, 3 repeated runs of the identical
// binary): "8248 byte(s) leaked in 398 allocation(s)" / "8290 byte(s)
// leaked in 400 allocation(s)" / "8248 byte(s) leaked in 398
// allocation(s)" — the LSan conservative-scan jitter `leaksAtLeast`'s own
// comment documents. Independently confirmed unaffected under the
// hand-built wrong variant (3 runs, same alternating pair) — row 1's
// top-level argument is never a NewExpr, so even the shallow wrong-variant
// check declines it exactly as the real fix does.
BEZEL_TEST(nested_record_string_field_leak_4237,
           nested_field_is_borrowed_identifier_not_admitted_no_uaf) {
    const char* prog =
        "module M; record Inner(S: string); record Outer(I: Inner); "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 200 do "
        "    var inner := new Inner(\"borrow\" + i.ToString()); "
        "    var outer := new Outer(inner); "
        "    acc += outer.I.S.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(leaksAtLeast(prog, 8000));
}

// Row 2: the nested field's OWN constructor argument is a borrow — `new
// Outer(new Inner(s))` where `s` is a plain identifier read of a live
// local, not a literal or a `+1` producer. The OUTER shape (Record field,
// NewExpr argument) IS recursion-eligible, so this is the load-bearing
// proof that the recursive call re-applies the string-literal-or-+1-
// producer rule at the NESTED level too, not just at the top level —
// exactly the depth-2 analogue of fix #3964's df2_outlives row. `s` is
// read again after `outer` is built, so a wrongly-admitted `outer` would
// free `s`'s reference out from under this later read (heap-use-after-
// free), strictly worse than the pre-existing safe leak.
//
// THIS is the row send-back FB-4237 found undiscriminating: MEASURED
// (this lane's own rebuild, same recipe as row 1) real fix: "8248 byte(s)
// leaked in 398 allocation(s)" / "8290 B / 400" / "8290 B / 400" across 3
// runs (same jitter). Under the hand-built wrong variant (shallow
// `dynamic_cast<ast::NewExpr>` + type match on `new Inner(s)`, recursive
// call dropped): CLEAN on all 3 runs — the leak vanishes to 0 B entirely,
// because the wrong variant's shallow check sees `new Inner(s)` IS a
// NewExpr of the right type and admits it WITHOUT ever inspecting `s`
// itself, so `outer` gets wrongly registered for release.
BEZEL_TEST(nested_record_string_field_leak_4237,
           nested_construction_borrowed_string_arg_not_admitted_no_uaf) {
    const char* prog =
        "module M; record Inner(S: string); record Outer(I: Inner); "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 200 do "
        "    var s := \"shared\" + i.ToString(); "
        "    var outer := new Outer(new Inner(s)); "
        "    acc += outer.I.S.Length; "
        "    acc += s.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(leaksAtLeast(prog, 8000));
}

// ===========================================================================
// Factory-method-path control (send-back FB-4237): the SAME borrowed-string-
// through-a-nested-construction shape as row 2, but reached via
// `isProvablyFreshRecordFactoryMethod`'s callee-body walk
// (`Factory.Make(s): Outer => new Outer(new Inner(s))`, called as
// `var o := Factory.Make(s);`) rather than a direct local `new`. This
// call-result path was UNTESTED by the original fix's suite (which only
// covers direct-local NewExpr registration) and behaves DIFFERENTLY from
// row 2: MEASURED (this lane's own rebuild, 3 runs, plain + repeated) the
// real fix is CLEAN here — no leak at all — where row 2's identical shape,
// one level removed via a direct local, safely leaks. Two paths, same
// shape, different answers; this row exists to PIN the currently-observed
// behavior so it cannot silently change, not to claim the discrepancy is
// fully understood.
//
// What IS traced (not merely asserted): `isFreshRecordConstructionOfExact
// Type` is the SAME shared authority both paths call — for THIS shape it
// returns false in both (an identifier read is never a +1 producer,
// whether the identifier is Main's local `s` or Factory.Make's OWN
// parameter `s`), so `isProvablyFreshRecordFactoryMethod` declines `o` for
// release exactly as `isFreshRecordConstructionOfExactType` declines
// `outer` in row 2 — the ELIGIBILITY answer is identical on both paths.
// The difference must therefore be in whether anything else independently
// disposes of the underlying string, which this fix's own eligibility
// proof has no visibility into either way.
//
// What is NOT fully traced: the exact codegen mechanism that makes the
// factory path clean rather than leaking (a partially-evidenced hypothesis,
// not an established fact). Filed as issue #4258 rather than guessed at
// here.
//
// This row DOES have teeth, independently of the open question: under the
// SAME hand-built wrong variant used for rows 1/2, this exact program goes
// from clean to a REPRODUCIBLE SEGV (measured 2/2 runs) — wrongly admitting
// `o` for release now double-frees the same string `s`'s own scope-end
// disposal also releases. The correct (fixed) behavior is asserted with
// `runChecked` (LSan-clean AND exit 0), the strongest oracle available —
// it fails on EITHER a reappeared leak or a reintroduced crash.
BEZEL_TEST(nested_record_string_field_leak_4237,
           factory_method_borrowed_string_arg_clean_no_uaf_open_question) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Inner(S: string); record Outer(I: Inner); "
        "class Factory "
        "  public static method Make(s: string): Outer "
        "    => new Outer(new Inner(s)); "
        "end Factory; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 300 do "
        "    var s := \"shared\" + i.ToString(); "
        "    var o := Factory.Make(s); "
        "    var n := o.I.S.Length; "
        "    var m := s.Length; "
        "    i += 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// Control: a NESTED CHOICE-typed field is UNCHANGED by this fix — still
// unconditionally disqualifies the whole outer construction (a choice's
// active-case payload is not statically known at this call site). Proves
// splitting the original combined `(Record || Choice)` disqualification
// into two separate `if`s did not accidentally widen the Choice arm.
// Still expected to safely leak (unaffected), so `didNotCrash` is the
// correct oracle here too.
BEZEL_TEST(nested_record_string_field_leak_4237,
           nested_choice_field_still_declines_unaffected_no_uaf) {
    const char* prog =
        "module M; choice Opt Some(V: string), None; end Opt; "
        "record Wrap(O: Opt); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    var w := new Wrap(Opt.Some(\"x\" + i.ToString())); "
        "    i += 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
}
