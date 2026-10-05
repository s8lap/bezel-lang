// Regression tests for issue #2457 slice 2 — the same declaration-less-
// builtin-type silent-fallthrough shape #2457 slice 1 (`a3d7468`) fixed for
// `Array<T>`/`OwningArray<T>`, applied to `Stack<T>`/`OwningStack<T>`/
// `Queue<T>`/`OwningQueue<T>` (stdlib §4.11). Confirmed live on `origin/main`
// before this fix:
//
//   var s := new Stack<int>(); s.Push(1);
//   var x := s.TotallyBogusStackMember123;
//
// compiled at exit 0 with zero diagnostics.
//
// Also confirmed live before this fix: #2465 (`72b4c70`)'s narrowed #605
// bare-type-parameter block had already turned `Stack<int>.Equals(itself)` /
// `.ToString()` / `.GetHashCode()` into a clean "declared via the universal
// Object contract... not yet implemented (tracked #2457)" diagnostic — a
// deliberate placeholder pointing at THIS slice. This fix replaces that
// placeholder diagnostic with the real §8.3/§8.5 implementation for exactly
// `Stack`/`OwningStack`/`Queue`/`OwningQueue` (`isAuditedElsewhere` in the
// #2465 block now excludes these four names).
//
// Per-member disposition (verified against docs/Bezel_Standard_Library.md
// §4.11/§4.1/§4.12/§5, not inferred from what the compiler special-cases):
//   - `Count`: already typed by a pre-existing property handler (shared with
//     Array/Set/Dictionary/LinkedList); the sole member for element counting.
//   - `.Length`: REJECTED per fix #5416 (stdlib §4.11, v0.315). Before the
//     fix, accepted as an undocumented alias for `Count` (same
//     `isCollectionTypeName`/`kArrayBackedCollections` structural dispatch
//     that gave Array its `.Length` before #4811). Fix removes it from the
//     allow-list so the stdlib's declared `Count` is the only accepted name.
//   - `.Reverse()`: NOT declared for Stack/Queue in stdlib §4.11 either (only
//     `Array<T>`'s own §4.4 declares an in-place `Reverse()`), but a second
//     undocumented-but-real structural alias found by THIS slice's own
//     adversarial pass: `tryLowerCollectionInstanceCall`
//     (LlvmBackend_Calls.cpp ~3160) implements it for ANY
//     `isCollectionTypeName` receiver with no per-type exclusion. Verified
//     VALUE-correct below (mutates the physical backing storage), not just
//     "doesn't crash" — exempted, not implemented.
//   - Every declared method (stdlib §4.11) and §5 LINQ operator: a CALL is
//     exempted via `calleeOfCallCtx_`; language spec §27.6 also sanctions a
//     BARE reference (`var f := s.Push;`), exempted via an explicit name
//     list transcribed fresh from stdlib §4.11/§4.12/§5 (mirrors slice 1's
//     identical two-mechanism defense).
//   - ToString/Equals/GetHashCode: `Stack<T>`/`OwningStack<T>`/`Queue<T>`/
//     `OwningQueue<T>` are real, documented `public class` types (stdlib
//     §4.11) with no override shown, and are genuine `bz_array_t*`-backed
//     heap objects with stable identity — the same §8.3/§8.5 mechanical
//     defaults slice 1 implemented for Array/OwningArray. IMPLEMENTED
//     (LlvmBackend_Calls2.cpp), not diagnosed.
//   - GetType/ReferenceEquals: exempted, matching every other category's
//     treatment (unaffected by this fix either way).
//
// Two new, DISTINCT bugs found by this slice's own adversarial pass and
// filed rather than fixed here (out of scope — this slice is about member
// RESOLUTION, not about the runtime correctness of already-legitimate
// operations): #2470 (a bare method reference to a builtin collection's
// instance method, e.g. `Array<T>.Add`/`Stack<T>.Push`, is a silent no-op
// when invoked — reproduces on `Array<T>` too, so it predates and is
// independent of this slice) and #2471 (`.GetEnumerator()` as an explicit
// CALL — not the `for x in coll do` desugaring, which is unaffected — is a
// non-functional enumerator for every collection except Array/OwningArray).
// A third, unrelated correctness bug (#2472: `Stack<T>`'s for-loop iterates
// insertion order, not the spec's top-to-bottom/pop order) was also found
// and filed, not fixed — a for-loop desugaring semantics bug, not a
// member-access resolution gap.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must
// be distinguished by asserting on the diagnostic TEXT, never by exit code
// or `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ── Bogus member rejected ───────────────────────────────────────────────

BEZEL_TEST(stack_queue_unresolved_member_2457, bogus_property_on_stack_rejected_stdlib_4_11) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); "
        "  var x := s.TotallyBogusStackMember123; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `TotallyBogusStackMember123` on `Stack<T>`"));
}

BEZEL_TEST(stack_queue_unresolved_member_2457, bogus_property_on_queue_rejected_stdlib_4_11) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var q := new Queue<int>(); "
        "  q.Enqueue(1); "
        "  var x := q.TotallyBogusQueueMember456; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program, "no member `TotallyBogusQueueMember456` on `Queue<T>`"));
}

BEZEL_TEST(stack_queue_unresolved_member_2457, bogus_property_on_owningstack_rejected) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var s := new OwningStack<Res>(); "
        "  s.Push(new Res(1)); "
        "  var x := s.TotallyBogusOwningStackMember; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `TotallyBogusOwningStackMember` on `OwningStack<T>`"));
}

BEZEL_TEST(stack_queue_unresolved_member_2457, bogus_property_on_owningqueue_rejected) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var q := new OwningQueue<Res>(); "
        "  q.Enqueue(new Res(1)); "
        "  var x := q.TotallyBogusOwningQueueMember; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `TotallyBogusOwningQueueMember` on `OwningQueue<T>`"));
}

// `s.CompareTo(s2)` is a CALL, so it is UNCONDITIONALLY exempted from this
// slice's own new reject block via `calleeOfCallCtx_` (mirroring slice 1's
// identical `isCallForm` exemption — call-form validation is deliberately
// left to the pre-existing, much older #920/#1112 (v0.155) call-site
// surface check at ~line 3979, `allowedNonOwningMethod`, which already
// rejects any call-form name outside its own generous allow-list uniformly
// across the WHOLE collection family, independent of this slice). `Compare
// To` is not on that allow-list either (Stack has no `IComparable<T>`
// conformance, stdlib §4.11) so the call was ALREADY rejected by that older
// mechanism before this slice, with its own "(§22.5 / §4.x)" wording — this
// is NOT a new diagnostic this slice adds.
//
// What THIS slice's `isAuditedElsewhere` update to the #2465 block DOES
// change, and what this test actually pins: before this slice, `CompareTo`
// on `Stack` produced TWO diagnostics for the same call — the older §22.5
// one AND #2465's own "declared via the universal Object contract... not
// yet implemented (tracked #2457)" placeholder (since #2465's block, before
// this slice, still treated Stack/Queue as "not yet audited" and fired
// unconditionally for its five affected names, `CompareTo` included). Now
// that Stack/OwningStack/Queue/OwningQueue are excluded from #2465's scope
// (this slice audited them), only the older, more precise diagnostic fires
// — the redundant/less-accurate #2465 placeholder text must be GONE.
BEZEL_TEST(stack_queue_unresolved_member_2457, compareto_call_no_longer_double_reported_by_placeholder_2465) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); "
        "  var s2 := new Stack<int>(); "
        "  s2.Push(2); "
        "  var c := s.CompareTo(s2); "
        "  return 0; "
        "end Main; end C;";
    // Still rejected (the pre-existing #920/#1112 call-site check, unrelated
    // to and unaffected by this slice).
    BEZEL_EXPECT_TRUE(hasError(program, "no member `CompareTo` on `Stack<T>`"));
    // The #2465 placeholder wording must no longer ALSO fire for Stack/Queue
    // now that this slice has audited them — the one observable difference
    // `isAuditedElsewhere`'s update makes for this call.
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(
            d.message.find("declared via the universal Object contract") ==
            std::string::npos);
}

// ── Legitimate members keep working, VALUE-correct ──────────────────────

// `.Count` — the declared member per stdlib §4.11 (fix #5416).
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_count_member_stack_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(7); s.Push(8); s.Push(9); "
        "  return s.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}

BEZEL_TEST(stack_queue_unresolved_member_2457, legit_count_member_queue_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var q := new Queue<int>(); "
        "  q.Enqueue(7); q.Enqueue(8); q.Enqueue(9); "
        "  return q.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}

// `.Reverse()` — the second undocumented-but-real alias this slice found:
// NOT in stdlib §4.11's declared surface, but a genuine, working in-place
// mutator (the same generic isCollectionTypeName dispatch Array's own
// Reverse uses). VALUE-correct: after pushing 1,2,3 (physical [1,2,3], top =
// index 2 = 3), Reverse() physically flips the backing storage to [3,2,1],
// so the next Pop() (which always reads index Count-1) returns the
// ORIGINAL first-pushed element, 1 — proving real mutation, not a no-op.
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_reverse_alias_stack_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  s.Reverse(); "
        "  return s.Pop(); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 1);
}

// A bare (uncalled) method reference — language spec §27.6.
//
// #2470 CORRECTION: this test originally asserted `run(program) == 0`
// ("compiles and doesn't crash") for the accepted reference, with a NOTE
// that the invocation was a known silent no-op tracked by #2470. #2470's
// own fix (sema/ExprTypecheck_Calls.cpp's Stack/Queue block) now REJECTS
// this bare-reference form outright instead of leaving that no-op: a
// clean-compile-then-silently-do-nothing is worse than an honest
// diagnostic (the standing protocol's framing), and `Push`/`Enqueue` on
// Stack<T>/Queue<T> have no real MethodSymbol for codegen's method-
// reference thunk to bind (they are dispatched structurally, exactly like
// Array<T>.Add). The CALL form (`s.Push(5)`, exercised by
// `legit_reverse_alias_value_correct` and others above) is unaffected.
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_bare_method_reference_now_rejected_2470) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  var f := s.Push; "
        "  f(5); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Push` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Stack<T>`"));
}

BEZEL_TEST(stack_queue_unresolved_member_2457, legit_bare_method_reference_queue_now_rejected_2470) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var q := new Queue<int>(); "
        "  var f := q.Enqueue; "
        "  f(5); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Enqueue` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Queue<T>`"));
}

// #2470: folded `isUniversalObjectMember` into the same reject on the
// Stack/Queue block too — shares the identical no-MethodSymbol root cause
// as `.Push`/`.Enqueue` (see the Array block's analogous test and sema
// comment for the confirmed-crashing repro shape on Array; the mechanism is
// receiver-shape-independent, so the same class of malformed-IR/ICE
// failure is expected here too, not separately re-confirmed per receiver).
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_bare_tostring_reference_stack_now_rejected_2470) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); "
        "  var f := s.ToString; "
        "  var str := f(); "
        "  return (if str = \"Stack\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `ToString` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Stack<T>`"));
}

// #2493 (stdlib §4.11; language spec §27.6): a BARE reference to `.Reverse`
// on Stack/Queue shares #2470's exact no-MethodSymbol no-op — `.Reverse` is a
// genuine callable on these receivers (an undocumented-but-working structural
// alias dispatched in codegen via the same isCollectionTypeName path
// Array<T>'s own §4.4 in-place Reverse uses, NOT one of stdlib §5's LINQ
// operators — see the §5 naming note, v0.195: the lazy `Reverse(): IEnumerable<T>`
// applies only where the receiver's static type has no instance `Reverse`, and
// codegen commits to the in-place mutation here). The CALL form `s.Reverse()`
// works (legit_reverse_alias_stack_value_correct above proves the real
// in-place mutation); the BARE form `var f := s.Reverse;` was a SILENT NO-OP
// — confirmed live on `origin/main` before this fix:
//
//   var s := new Stack<int>(); s.Push(1); s.Push(2); s.Push(3);
//   var f := s.Reverse; f(); return s.Pop();
//
// returned `3` (the no-op: `s` was never reversed, so Pop returns the
// un-flipped top) where a working reverse returns `1` (the original
// first-pushed element), with the diagnostic `lowerLocalDecl's initializer
// expression failed to lower to a value for this local's slot (degraded to a
// zero-initialized value) — #2434` — the identical codegen degradation
// #2470 documented for `s.Push`. sema now rejects the bare reference at the
// reference site (folding `.Reverse` into the same #2470 reject), so the
// forbidden clean-compile-then-silently-do-nothing outcome is replaced with
// an honest "not yet supported" diagnostic. This test FAILS without the fix
// (no error — the program compiled and silently no-op'd) and PASSES with it
// (the reject fires).
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_bare_reverse_reference_stack_now_rejected_2493) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var f := s.Reverse; "
        "  f(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Reverse` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Stack<T>`"));
}

// #2493 Queue twin — same root cause, same reject.
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_bare_reverse_reference_queue_now_rejected_2493) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var q := new Queue<int>(); "
        "  q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "  var f := q.Reverse; "
        "  f(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Reverse` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Queue<T>`"));
}

// #2493 adversarial: the CALL form `s.Reverse()` MUST still compile and work
// after the bare-reference reject (the reject is gated on `!isCallForm`).
// Queue twin of legit_reverse_alias_stack_value_correct above: after
// enqueuing 1,2,3 (front=1, back=3), the in-place Reverse() flips the
// backing storage so Dequeue() (reads the front) returns the ORIGINAL back
// element, 3 — proving the call form's real mutation survives the fix.
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_reverse_call_queue_value_correct_2493) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var q := new Queue<int>(); "
        "  q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "  q.Reverse(); "
        "  return q.Dequeue(); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}

// #2492 (stdlib §5; language spec §27.6): a BARE reference to a stdlib §5
// LINQ extension operator (`Where`/`Select`/`Take`/...) on Stack<T>/Queue<T>
// (and the OwningStack/OwningQueue twins) shares the identical #2470/#2493
// silent-no-op root cause — the operator is dispatched STRUCTURALLY by name
// in codegen's tryLowerLinqOperatorCall, never through a real MethodSymbol,
// so sema left `ma`'s exprType UNSET and codegen's #2434 fallback degraded
// the local to a zero-init value. Confirmed live on origin/main before this
// fix: `var f := s.Where;` on Stack<int> compiled CLEAN (no sema diagnostic
// — `isLinqOperator` was exempted from BOTH the #2470 bare-reference reject
// and the "no member" reject) and emitted only the codegen-internal `...
// degraded to a zero-initialized value -- #2434`. sema now folds
// `isLinqOperator` into the same #2470 reject. This test was originally
// `legit_linq_bare_method_reference_not_regressed` asserting `run(program)
// == 0`, but that was VACUOUS — it never CALLED `f`, so it passed whether
// the bare reference genuinely worked OR silently no-op'd (the defect).
// Repurposed to assert the reject (FAILS without this fix: no error, silent
// no-op; PASSES with it: reject fires). The "not yet supported" message
// (NOT "no member") preserves the original intent: `Where` IS recognized as
// a genuine stdlib §5 LINQ member (in `isLinqOperator`).
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_bare_linq_reference_stack_now_rejected_2492) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var f := s.Where; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Where` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Stack<T>`"));
}

// #2492 Queue twin — same root cause, same reject.
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_bare_linq_reference_queue_now_rejected_2492) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var q := new Queue<int>(); "
        "  q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "  var f := q.Where; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Where` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Queue<T>`"));
}

// `.ToArray` bare reference — found MISSING from this slice's own
// isLinqOperator list by post-commit review (a false reject: `ToArray` is a
// genuine stdlib §5 `IEnumerable<T>` extension with no per-type override on
// Stack/Queue, unlike `Array<T>`/`OwningArray<T>`, which separately declare
// their OWN native `ToArray()` — stdlib §4.4/§4.5 — masking the identical
// omission in slice 1's list). #2492 folds it into the same bare-reference
// reject as the other LINQ operators: the CALL form (`s.ToArray()`) is
// unaffected (exempted via `!isCallForm`, then the pre-existing #920/#1112
// `allowedNonOwningMethod` list, which DOES include `ToArray` — guarded by
// legit_toarray_call_form_value_correct below) — only the BARE spelling
// silently no-op'd before, now honestly rejected. The "not yet supported"
// message (NOT "no member") confirms `ToArray` IS in `isLinqOperator`
// (preserving the original #2457-slice-2 intent). Repurposed from the prior
// vacuous `run(program) == 0` assertion (which never called `f`).
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_toarray_bare_reference_stack_now_rejected_2492) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var f := s.ToArray; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `ToArray` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Stack<T>`"));
}

// `.MinOrNull`/`.MaxOrNull` bare reference — a SECOND gap this slice's own
// independent re-derivation of stdlib §5/§5.1 found (not caught by the
// coordinator's review, which flagged only `ToArray`): both are genuine
// members of the `IEnumerable<T> where T: IComparable<T>` extension (§5.1)
// with no per-type override on Stack. #2492 folds them into the same
// bare-reference reject: the CALL form is unaffected, the BARE form is now
// honestly rejected (was a silent no-op). The "not yet supported" message
// (NOT "no member") confirms both names ARE in `isLinqOperator` (preserving
// the original intent). Repurposed from the prior vacuous `run == 0` form.
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_minornull_maxornull_bare_reference_stack_now_rejected_2492) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var f := s.MinOrNull; "
        "  var g := s.MaxOrNull; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `MinOrNull` (language spec \xC2\xA7"
        "27.6) is not yet supported on `Stack<T>`"));
}

// #2492 adversarial VALUE-correctness: the CALL form `s.Where(lambda).First()`
// MUST still compile and return the correct value after the bare-reference
// reject (the reject is gated on `!isCallForm`). From Stack pushed 1,2,3,4
// (top=4), `Where(x > 2)` yields the elements > 2 and `First()` returns the
// first of them — proving the call form's real LINQ dispatch survives the
// fix (the control for the bare-form reject above). Mirrors the Array twin.
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_where_call_form_stack_value_correct_2492) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); s.Push(4); "
        "  var e := s.Where(lambda(x) => x > 2); "
        "  return e.First(); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}

// `.ToArray()` CALL form, VALUE-correct — proves the older #920/#1112
// call-site validator's separate `ToArray` allow-list entry and this
// fix's `isCallForm` exemption compose correctly for Stack (not just
// "doesn't crash": materializes the exact elements).
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_toarray_call_form_value_correct) {
    const std::string program =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); "
        "  using var a := s.ToArray(); "
        "  return a.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 2);
}

// Call-form methods across the declared surface still work — sampled, not
// exhaustive (the broad `stack_`/`queue_`/`collection` suites already
// exercise the full call surface; this is a direct guard on THIS fix's own
// reject condition), VALUE-correct.
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_call_form_methods_stack_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var found := s.Contains(2); "
        "  var top := s.Peek(); "
        "  var popped := s.Pop(); "
        "  s.Clear(); "
        "  if not found then return 1; end; "
        "  if top <> 3 then return 2; end; "
        "  if popped <> 3 then return 3; end; "
        "  if s.Count <> 0 then return 4; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

BEZEL_TEST(stack_queue_unresolved_member_2457, legit_call_form_methods_queue_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var q := new Queue<int>(); "
        "  q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "  var found := q.Contains(2); "
        "  var front := q.Peek(); "
        "  var dequeued := q.Dequeue(); "
        "  q.Clear(); "
        "  if not found then return 1; end; "
        "  if front <> 1 then return 2; end; "
        "  if dequeued <> 1 then return 3; end; "
        "  if q.Count <> 0 then return 4; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// OwningStack/OwningQueue's own extra declared method — Drain() — plus
// TryPop/TryPeek/TryDequeue's `out` form.
BEZEL_TEST(stack_queue_unresolved_member_2457, legit_owningstack_drain_and_try_forms_value_correct) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var s := new OwningStack<Res>(); "
        "  s.Push(new Res(1)); "
        "  s.Push(new Res(2)); "
        "  var top: Res? := null; "
        "  var ok := s.TryPeek(out top); "
        "  if not ok then return 1; end; "
        "  if top!.Tag <> 2 then return 2; end; "
        "  var n := 0; "
        "  for var r in s.Drain() do n := n + r.Tag; r.Dispose(); end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}

// ToString: implemented as the fixed literal, VALUE-correct.
//
// fix #3119 (spec §8.5, v0.252; stdlib §2.13 rule 1 + rule 3): the literal
// is the QUALIFIED module path with the §2.13 rule 3 generic-args tail —
// `Stack<int>` → "System.Collections.Generic.Stack<int>",
// `OwningStack<Res>` (Res is module M) →
// "System.Collections.Generic.OwningStack<M.Res>", etc.
BEZEL_TEST(stack_queue_unresolved_member_2457, tostring_implemented_stack_literal) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(5); "
        "  var t := s.ToString(); "
        "  return (if t = \"System.Collections.Generic.Stack<int>\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

BEZEL_TEST(stack_queue_unresolved_member_2457, tostring_implemented_queue_literal) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var q := new Queue<int>(); "
        "  q.Enqueue(5); "
        "  var t := q.ToString(); "
        "  return (if t = \"System.Collections.Generic.Queue<int>\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

BEZEL_TEST(stack_queue_unresolved_member_2457, tostring_implemented_owningstack_literal) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var s := new OwningStack<Res>(); "
        "  s.Push(new Res(1)); "
        "  var t := s.ToString(); "
        "  return (if t = \"System.Collections.Generic.OwningStack<M.Res>\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

BEZEL_TEST(stack_queue_unresolved_member_2457, tostring_implemented_owningqueue_literal) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var q := new OwningQueue<Res>(); "
        "  q.Enqueue(new Res(1)); "
        "  var t := q.ToString(); "
        "  return (if t = \"System.Collections.Generic.OwningQueue<M.Res>\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// Equals/GetHashCode: implemented via direct identity, VALUE-correct
// (self-equal, two distinct instances unequal, hash stable across calls).
// This is also the exact #2465 crash-repro shape (`Stack<int>.Equals` used
// in a branch condition previously IR-verification-crashed the compiler
// before #2465's placeholder reject, which this fix now replaces with a
// real implementation).
BEZEL_TEST(stack_queue_unresolved_member_2457, equals_and_gethashcode_identity_semantics_stack) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s1 := new Stack<int>(); "
        "  var s2 := new Stack<int>(); "
        "  var selfEq := s1.Equals(s1); "
        "  var crossEq := s1.Equals(s2); "
        "  var h1a := s1.GetHashCode(); "
        "  var h1b := s1.GetHashCode(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "
        "  if h1a <> h1b then return 3; end; "
        "  if selfEq then return 42; else return 4; end; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

BEZEL_TEST(stack_queue_unresolved_member_2457, equals_and_gethashcode_identity_semantics_queue) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var q1 := new Queue<int>(); "
        "  var q2 := new Queue<int>(); "
        "  var selfEq := q1.Equals(q1); "
        "  var crossEq := q1.Equals(q2); "
        "  var h1a := q1.GetHashCode(); "
        "  var h1b := q1.GetHashCode(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "
        "  if h1a <> h1b then return 3; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}
