// fix #4232 (§9.7 "Records and disposal" / §28.9 "Runtime-managed types" /
// §14.1 "Generic declarations", v0.298): SIGSEGV (stack overflow, ~43,500
// identical frames measured via gdb) in `Emitter::recordHasStringFields`
// (LlvmBackend_impl.hpp) when a still-GENERIC method declares a local whose
// type nests the method's own rigid (unbound) type parameter inside a
// record two-or-more levels deep, constructed from a one-level argument —
// the issue's own minimal repro is:
//
//   record Box<T>(Value: T);
//   class Util
//     public static method Rewrap<T>(x: Box<T>): int
//       var y: Box<Box<T>> := new Box<Box<T>>(x);
//       return 0;
//     end Rewrap;
//   end Util;
//
// ROOT CAUSE: the #2405/#3945 comments' termination argument ("sema rejects
// a record that transitively contains itself by value, so this walk is over
// a finite DAG") is a DECLARATION-graph guarantee; it says nothing about a
// still-generic method's own rigid type parameter. `RecordSubstGuard` binds
// `Box`'s own param name "T" to `resolveTypeParamRef(Box<T>)` — and since
// `Box<T>` already has a `declaration`, `resolveTypeParamRef` returns it
// UNCHANGED, so `genericSubstSemaTy_["T"]` becomes the exact TypeRef node
// the recursion is about to visit next. One level down, the record's OWN
// field `Value: T` resolves through that same binding back to the SAME
// TypeRef pointer — the recursive call target is pointer-identical to the
// current frame, forever: the substitution's fixed point is "T -> Box<T>",
// reachable only because `T` never resolves to anything ground here (sema
// correctly accepts the program — `T` need not resolve until `Rewrap` is
// itself instantiated at a call site).
//
// FIX: `recordHasStringFields` now threads a PATH guard
// (`std::unordered_set<const sema::TypeRef*>& seen`, insert-on-entry /
// erase-on-exit through its own `recordHasStringFieldsOnPath` recursion) —
// matching the established shape of `common::isBorrowBearingRecordCommon`'s
// own `seen` (OwnershipFacts.cpp): a PATH guard, not a permanent visited-set
// memoization (a sibling field reached through a different branch must
// still be free to re-classify a TypeRef an unrelated, already-completed
// branch also touched — #2145's identical "benign sibling first" hazard).
// Keyed on the TypeRef INSTANTIATION (`tr`), not the record's TypeSymbol
// declaration: two different instantiations of the same generic record
// (`Box<int>` vs. `Box<string>`) must not collide — `TypeSymbol*` is shared
// by every instantiation, `TypeRef*` is not (mirrors #3933's identical
// correction to `isBorrowBearingRecordCommon`'s own guard key).
//
// MEASURED (this file's own FAIL-WITHOUT/PASS-WITH cycle, fixing lane,
// isolated worktree at 6f46a73 for FAIL-WITHOUT, this tree for PASS-WITH):
// gdb confirms the crash reproduces byte-for-byte at 6f46a73 (SIGSEGV,
// exit 139, "the monitored command dumped core" under `timeout`) with the
// exact frame signature the issue reports:
//   #0 RecordSubstGuard::RecordSubstGuard -> lowerTypeRef -> isNullableValueTy
//   #1..N  Emitter::recordHasStringFields (LlvmBackend_impl.hpp:10850),
//          recursing into itself via the SAME line, ~43,500 times, until the
//          stack is exhausted.
// See tests/README.md for the `runAot` harness this file uses exclusively.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
// LSan/ASan-clean oracle (see generic_record_string_field_leak_3945_test.cpp
// for the same pattern): `ran` is false on EITHER a sanitizer finding OR a
// crash (SIGSEGV routes through the same fork-isolated `ran=false` path,
// §9's process-isolation guarantee — CLAUDE.md), so this single oracle
// proves "compiled, ran, exited cleanly, leaked/corrupted nothing" all at
// once — the "clean outcome", not merely "did not crash".
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
// fix #3964 review's pattern (see generic_record_string_field_leak_3945_test.cpp):
// leak-TOLERANT crash/UAF/double-free oracle (detect_leaks=0), used ONLY for
// the three rows below that hit issue 4237's separate, PRE-EXISTING local-
// variable disposal gap (a documented, safe-direction LEAK — not a crash,
// not this fix's to close). A strict `runAotChecked` would report `ran=false`
// for a known leak exactly as it would for a real corruption, so it cannot
// distinguish the two; this can.
bool didNotCrash(const std::string& program) {
    auto r = bezel::test::runAotCheckedLenient(program);
    return r.ran;
}
}  // namespace

// ===========================================================================
// The issue's own minimal repro, verbatim. Pre-fix: SIGSEGV (measured,
// exit 139, at 6f46a73 — see this file's header). Post-fix: compiles, runs,
// exits 0. `runChecked` also confirms no ASan/LSan finding on the path the
// crash used to take (Box<int> carries no string, so this is chiefly a
// crash-freedom pin; the value-bearing rows below pin the CORRECT VALUE
// through the same recursive shape, per LANE_RULES: a crash-freedom oracle
// alone is not enough when a backstop diagnostic could otherwise mask it —
// not applicable to a SIGSEGV, but the value rows below remove any doubt).
// ===========================================================================
BEZEL_TEST(record_string_fields_generic_recursion_4232, issue_repro_exact_no_segv_clean_run) {
    const char* prog =
        "module M; record Box<T>(Value: T); "
        "class Util "
        "  public static method Rewrap<T>(x: Box<T>): int "
        "    var y: Box<Box<T>> := new Box<Box<T>>(x); "
        "    return 0; "
        "  end Rewrap; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<int> := new Box<int>(5); "
        "    return Util.Rewrap(p); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 0);
    BEZEL_EXPECT_EQ(run(prog), 0);
}

// ===========================================================================
// Depth 4 / 5 / 6: the SAME self-referential fixed point ("T -> Box<T>"),
// reached only after several GENUINELY DISTINCT (non-cyclic) TypeRef frames
// walk down through `Box<Box<...<T>...>>`'s syntactic nesting first — each
// `var yN: Box<...N times...<T>> := new Box<...>(y(N-1))` is its own AST
// occurrence (a distinct TypeRef pointer), so the guard must NOT fire on any
// of those legitimate frames, and must still catch the cycle once the
// recursion bottoms out at the innermost `Box<T>` shape. Each method returns
// the bare `T` unwrapped all the way back out through every layer
// (`yN.Value.Value...Value`), proving VALUE correctness (not just
// crash-freedom) survives the guard at every depth: `Rewrap4/5/6` must
// return exactly the int the caller boxed.
// ===========================================================================
BEZEL_TEST(record_string_fields_generic_recursion_4232, depth4_generic_method_local_round_trips_value) {
    const char* prog =
        "module M; record Box<T>(Value: T); "
        "class Util "
        "  public static method Rewrap4<T>(x: Box<T>): T "
        "    var y1: Box<Box<T>> := new Box<Box<T>>(x); "
        "    var y2: Box<Box<Box<T>>> := new Box<Box<Box<T>>>(y1); "
        "    var y3: Box<Box<Box<Box<T>>>> := new Box<Box<Box<Box<T>>>>(y2); "
        "    return y3.Value.Value.Value.Value; "
        "  end Rewrap4; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<int> := new Box<int>(5); "
        "    return Util.Rewrap4(p); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
    BEZEL_EXPECT_EQ(run(prog), 5);
}

BEZEL_TEST(record_string_fields_generic_recursion_4232, depth5_generic_method_local_round_trips_value) {
    const char* prog =
        "module M; record Box<T>(Value: T); "
        "class Util "
        "  public static method Rewrap5<T>(x: Box<T>): T "
        "    var y1: Box<Box<T>> := new Box<Box<T>>(x); "
        "    var y2: Box<Box<Box<T>>> := new Box<Box<Box<T>>>(y1); "
        "    var y3: Box<Box<Box<Box<T>>>> := new Box<Box<Box<Box<T>>>>(y2); "
        "    var y4: Box<Box<Box<Box<Box<T>>>>> := new Box<Box<Box<Box<Box<T>>>>>(y3); "
        "    return y4.Value.Value.Value.Value.Value; "
        "  end Rewrap5; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<int> := new Box<int>(7); "
        "    return Util.Rewrap5(p); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 7);
    BEZEL_EXPECT_EQ(run(prog), 7);
}

BEZEL_TEST(record_string_fields_generic_recursion_4232, depth6_generic_method_local_round_trips_value) {
    const char* prog =
        "module M; record Box<T>(Value: T); "
        "class Util "
        "  public static method Rewrap6<T>(x: Box<T>): T "
        "    var y1: Box<Box<T>> := new Box<Box<T>>(x); "
        "    var y2: Box<Box<Box<T>>> := new Box<Box<Box<T>>>(y1); "
        "    var y3: Box<Box<Box<Box<T>>>> := new Box<Box<Box<Box<T>>>>(y2); "
        "    var y4: Box<Box<Box<Box<Box<T>>>>> := new Box<Box<Box<Box<Box<T>>>>>(y3); "
        "    var y5: Box<Box<Box<Box<Box<Box<T>>>>>> := "
        "        new Box<Box<Box<Box<Box<Box<T>>>>>>(y4); "
        "    return y5.Value.Value.Value.Value.Value.Value; "
        "  end Rewrap6; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<int> := new Box<int>(9); "
        "    return Util.Rewrap6(p); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 9);
    BEZEL_EXPECT_EQ(run(prog), 9);
}

// ===========================================================================
// A DIFFERENT record (not "Box", not "Value") hitting the identical fixed
// point — rules out any accidental coupling of the fix to the issue's own
// identifier spelling.
// ===========================================================================
BEZEL_TEST(record_string_fields_generic_recursion_4232, self_referential_generic_different_record_name_no_segv) {
    const char* prog =
        "module M; record Wrap<U>(Payload: U); "
        "class Helper "
        "  public static method Nest<U>(item: Wrap<U>): U "
        "    var doubled: Wrap<Wrap<U>> := new Wrap<Wrap<U>>(item); "
        "    return doubled.Payload.Payload; "
        "  end Nest; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var q: Wrap<int> := new Wrap<int>(11); "
        "    return Helper.Nest(q); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 11);
    BEZEL_EXPECT_EQ(run(prog), 11);
}

// ===========================================================================
// A MUTUALLY-RECURSIVE PAIR of generic records (A<T>, B<T>, both declaring
// their own type parameter "T" — the same bare param NAME `genericSubst_`
// keys on). `B<A<T>>`'s field resolves to `A<T>`; entering `A<T>`'s own
// frame, its field `Value: T` resolves through the SAME per-name binding
// back to the identical `A<T>` TypeRef the frame is already processing —
// the cycle alternates between TWO DIFFERENT record declarations rather
// than one record recursing into itself, proving the guard's `TypeRef*` key
// (not the record's identity) is what actually breaks the cycle.
// ===========================================================================
BEZEL_TEST(record_string_fields_generic_recursion_4232, mutually_recursive_pair_no_segv) {
    const char* prog =
        "module M; record A<T>(Value: T); record B<T>(Value: T); "
        "class Util "
        "  public static method Mix<T>(x: A<T>): T "
        "    var y: B<A<T>> := new B<A<T>>(x); "
        "    return y.Value.Value; "
        "  end Mix; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: A<int> := new A<int>(13); "
        "    return Util.Mix(p); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 13);
    BEZEL_EXPECT_EQ(run(prog), 13);
}

// ===========================================================================
// Proves the guard does not merely suppress the crash by giving up on the
// whole classification: `Box<Box<T>>` here ALSO carries a genuine, directly
// (non-recursively) string-typed sibling field `Tag`. `collectRecordStorage`
// visits `Value` (the cyclic arm — now correctly guarded to `false`) BEFORE
// `Tag`, so `recordHasStringFields` must keep walking past the guarded arm
// and still find `Tag`, registering it for retain/release. Loop-amplified
// (300 iterations, LANE policy). `collectRecordStorage` visits `Value` (the
// cyclic arm — now correctly guarded to `false`) BEFORE `Tag`, so this proves
// the walk keeps going past the guarded arm rather than giving up on the
// whole classification once it fires.
//
// ⚠ MEASURED DURING THIS FIX'S OWN TESTING (not #4232's bug): this program's
// `Tag` string is NOT actually released at `y`'s scope end — a genuine,
// PRE-EXISTING, SEPARATE leak, reproducible even with NO generics at all
// (isolated minimal repro: `record Inner(S: string); record Outer(I: Inner);`
// `var o := new Outer(new Inner("hi"));` in a loop — 300 iterations, 11290 B
// / 600 allocations leaked under LSan, measured on this same build). The
// classification (`recordHasStringFields`/`isRecordWithStringFieldsTypeRef`)
// IS correct — the local-variable candidate-registration site
// (`LlvmBackend_Setup.cpp` ~27614-27621) calls it and it does find `Tag` (and
// `Inner.S`) transitively; the leak instead traces to
// `isFreshRecordConstructionOfExactType`'s own argument-eligibility proof,
// which appears not to recognize a NESTED record-typed constructor argument
// (itself a fresh `NewExpr`, e.g. `Box<T>` passed as `x`, or `Inner(...)` as
// `Outer`'s field) as an eligible "+1 producer" shape — declining the whole
// local's release registration, falling to the documented safe (leak, not
// UAF) direction. Filed as issue 4237 (not verified further here — OUT OF
// #4232's scope per LANE_RULES "do NOT widen scope"). This row therefore
// uses the leak-TOLERANT `didNotCrash` oracle (proves no crash/UAF/double-
// free) paired with the PLAIN oracle for value correctness — NOT a strict
// leak-free assertion, which would conflate this pre-existing, separate gap
// with #4232's own fix. `acc mod 100` keeps the return value AOT-exit-status
// safe (< 256; CLAUDE.md) — measured 90 (`"outer" + i.ToString()` summed over
// i in [0,300) mod 100, Python-verified during authoring).
// ===========================================================================
BEZEL_TEST(record_string_fields_generic_recursion_4232, outer_string_field_detected_after_cycle_break) {
    const char* prog =
        "module M; record Box<T>(Value: T, Tag: string); "
        "class Util "
        "  public static method Rewrap<T>(x: Box<T>): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 300 do "
        "      var y: Box<Box<T>> := new Box<Box<T>>(x, \"outer\" + i.ToString()); "
        "      acc += y.Tag.Length; "
        "      i += 1; "
        "    end; "
        "    return acc mod 100; "
        "  end Rewrap; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<int> := new Box<int>(5, \"inner\"); "
        "    return Util.Rewrap(p); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    BEZEL_EXPECT_EQ(run(prog), 90);
}

// ===========================================================================
// GUARD-KEY CONTROL (per review steering: "two different instantiations of
// the same generic record must not be treated as a cycle — test that
// explicitly"). `Box<Box<string>>` is a fully GROUND, finite, ordinary
// nested generic instantiation — no rigid method type parameter anywhere,
// so it does NOT crash even pre-fix (verified: T resolves all the way to
// the concrete `string` primitive at the innermost frame, a genuine fixed
// point, not a loop). Its purpose is not FAIL-WITHOUT/PASS-WITH — it is a
// control intended to pin the guard's KEY CHOICE (`TypeRef*` instantiation
// rather than the record's shared `TypeSymbol*` "Box").
//
// ⚠ CORRECTED per the #4232 review (R18): THIS ROW DOES NOT ACTUALLY PIN
// THAT CHOICE, and an earlier version of this comment claimed it did.
// Measured: patching the guard to `TypeSymbol*` keying and diffing the
// emitted IR (`BEZEL_DUMP_IR=1`) for this exact program against the shipped
// fix gives a BYTE-IDENTICAL module (`diff` = 0 lines), and the program's
// leak footprint is identical under both keyings (12190 B / 600 allocations
// at loop x300). Two independent reasons: (a) the assertion is a field READ
// (`b.Value.Value.Length`), and `recordHasStringFields` only ever decides
// DISPOSAL, never value correctness — so no keying can change this answer;
// (b) the disposal difference that WOULD be observable is masked by the
// pre-existing eligibility gap tracked as #4237 and its broader class-field
// cousin, which declines the local's disposal registration regardless of how
// the guard classifies it.
//
// The KEY CHOICE is still correct — traced directly: `Box<Box<string>>`'s
// field `Value` resolves to `Box<string>`, which shares the `TypeSymbol`
// "Box" with the outer frame, so a `TypeSymbol*`-keyed guard collides
// immediately even though the two instantiations are semantically distinct.
// But that collision could not be made OBSERVABLE through execution in any
// construction tried (plain locals, helper-returned locals, borrowed
// parameters, class fields) — every path is blocked by an unrelated
// pre-existing gap. Whoever fixes #4237 should revisit this row and give it
// real discriminating power. Until then it is a smoke test, not a control.
// ===========================================================================
BEZEL_TEST(record_string_fields_generic_recursion_4232, distinct_generic_instantiations_not_treated_as_cycle) {
    const char* prog =
        "module M; record Box<T>(Value: T); "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box<Box<string>>(new Box<string>(\"hello\")); "
        "    return b.Value.Value.Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
    BEZEL_EXPECT_EQ(run(prog), 5);
}

// ===========================================================================
// "Benign sibling first" (mirrors isBorrowBearingRecordCommon's #2145
// concern, OwnershipFacts.cpp): `Pair`'s first field `A: Box<int>` carries
// no string; a PERMANENT visited-set (memoization instead of a path guard)
// would leave "Box" (or the `Box<int>` TypeRef) marked visited after `A`
// completes, and the SECOND field `B: Box<string>` — a DIFFERENT, string-
// bearing instantiation reached only as a sibling, not nested inside `A`'s
// path — would then be vacuously skipped. `recordHasStringFieldsOnPath`'s
// erase-on-exit is what lets `B` be freshly reclassified — this row proves
// the CLASSIFICATION side of that (via the plain-oracle value, and via
// `didNotCrash`).
//
// ⚠ Uses the SAME leak-tolerant oracle as
// `outer_string_field_detected_after_cycle_break` just above, for the
// IDENTICAL reason: this exact shape (a record field itself constructed as a
// nested `new Box<string>(...)`) hits issue 4237's separate, pre-existing
// local-disposal eligibility gap, not #4232's. `acc mod 100` measured 90
// (Python-verified during authoring).
// ===========================================================================
BEZEL_TEST(record_string_fields_generic_recursion_4232, sibling_fields_benign_first_then_string_bearing) {
    const char* prog =
        "module M; record Box<T>(Value: T); record Pair(A: Box<int>, B: Box<string>); "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 300 do "
        "      var p := new Pair(new Box<int>(i), new Box<string>(\"s\" + i.ToString())); "
        "      acc += p.B.Value.Length; "
        "      i += 1; "
        "    end; "
        "    return acc mod 100; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    BEZEL_EXPECT_EQ(run(prog), 90);
}

// ===========================================================================
// Wide diamond (branch 3, depth 4: Top -> Node -> Mid -> Leaf, each level
// with 3 same-typed fields, `Leaf` reached by many distinct paths) —
// compile-time sanity per review steering. Entirely GROUND (no generics at
// all), so this is not a #4232 crash risk in itself; it pins that the path
// guard (erase-on-exit, no cross-branch memoization) does not misclassify a
// legitimate, finite, heavily-shared DAG as cyclic, and that walking it
// compiles and runs promptly rather than hanging.
//
// ⚠ CORRECTED per the #4232 review (R18): an earlier version of this comment
// said "81 leaf visits, unmemoized". That is wrong — `Leaf` HAS a string
// field, so every level's field loop short-circuits (`found = true; break`)
// on its FIRST field. The actual traversal is O(depth), not O(branch^depth),
// and this row never exercises the exponential path at all. The row is still
// a valid DAG-not-cycle smoke test; it is not a blowup test. The genuine
// compile-time blowup is measured and tracked separately as #4234 (branch=3
// depth=6 measured at ~21.9 s / 542 MB, IDENTICAL on this fix and on its
// parent `6f46a73` — this fix neither causes nor worsens it).
//
// ⚠ Uses the SAME leak-tolerant oracle as the two tests above, for the same
// reason: a NESTED record-typed constructor argument (`new Mid(leaf, leaf,
// leaf)`, `new Node(mid, mid, mid)`, `new Top(node, node, node)`) hits issue
// 4237's separate, pre-existing local-disposal eligibility gap at every
// level of this diamond. `acc mod 100` measured 90 (Python-verified during
// authoring).
// ===========================================================================
BEZEL_TEST(record_string_fields_generic_recursion_4232, wide_diamond_branch3_depth4_compiles_and_runs_promptly) {
    const char* prog =
        "module M; "
        "record Leaf(S: string); "
        "record Mid(A: Leaf, B: Leaf, C: Leaf); "
        "record Node(A: Mid, B: Mid, C: Mid); "
        "record Top(A: Node, B: Node, C: Node); "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 100 do "
        "      var leaf := new Leaf(\"s\" + i.ToString()); "
        "      var mid := new Mid(leaf, leaf, leaf); "
        "      var node := new Node(mid, mid, mid); "
        "      var top := new Top(node, node, node); "
        "      acc += top.A.A.A.S.Length; "
        "      i += 1; "
        "    end; "
        "    return acc mod 100; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    BEZEL_EXPECT_EQ(run(prog), 90);
}
