// fix #3945 (§9.7 "Records and disposal" / §28.9 "Runtime-managed types" /
// §14.1 "Generic declarations", v0.294): a generic record's string-owning-
// field disposal walk (isStringStorageMember / recordHasStringFields /
// emitRecordStringFieldReleases_impl, LlvmBackend_impl.hpp) resolved each
// storage member's type via the BARE `recordMemberType`. A bare type
// parameter (`X: T`) always has `kind == Named, declaration == nullptr`
// regardless of what `T` is instantiated to — `recordMemberType` never
// substitutes it — so a generic record field declared at a bare type
// parameter was NEVER recognized as string-owning, no matter its
// instantiation: every string constructed through such a field leaked.
//
// Same defect family as fix #3899 (patternMatches's DestructuringPattern
// branch) and fix #3944 (buildRecordValue's body-field default-init loop),
// but here the fix is NOT a simple accessor swap: none of
// isStringStorageMember/recordHasStringFields/emitRecordStringFieldReleases_impl
// receive a per-instantiation TypeRef at all (only a TypeSymbol), so there is
// no active generic substitution to consult even after swapping to
// `recordMemberTypeResolved`. The fix widens `recordHasStringFields` and
// `emitRecordStringFieldReleases_impl` to take the record's own
// per-instantiation TypeRef and install a `RecordSubstGuard` for it (a no-op
// for a non-generic TypeRef) — every existing call site already held that
// TypeRef, it was simply being discarded (`->declaration`) before reaching
// these functions.
//
// MEASURED (this file's own FAIL-WITHOUT/PASS-WITH cycle, run by the fixing
// lane, --probe --asan, 300 iterations): pre-fix, 13046 bytes leaked in 598
// allocations (issue's own repro measured 13090 B / 600 allocations — same
// signature, prefix count is build/content-length dependent); post-fix,
// clean exit under LSan. Verified via `runAotChecked`, which sets
// `opts.sanitizeAddress = true` on the GENERATED program (not a hand-rolled
// runner that defaults sanitizeAddress to false — see issue #3917).

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
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
// fix #3964 review: is the generated program free of a UAF / double-free /
// OOB in ASan's generated-code instrumentation? Uses runAotCheckedLenient
// (detect_leaks=0), NOT plain runAotChecked — runAotChecked treats ANY
// sanitizer finding as `ran=false`, a KNOWN, ACCEPTABLE leak (LSan's own
// exit-time report) included, so it cannot distinguish "still safely leaks"
// from "now corrupts memory". The lenient variant disables ONLY the exit-
// time leak check; the generated code stays ASan-instrumented throughout
// execution, so a genuine use-after-free read or an invalid/double free is
// still caught (`ran=false`) — this is the oracle for the five df*/cls*
// regressions below, which are expected to still leak safely (the pre-
// existing, undisturbed behavior) but must NEVER crash. Paired with a
// SEPARATE plain-`runAot` value assertion, matching LANE_RULES' "prove
// crash-freedom AND value-correctness" pattern.
bool didNotCrash(const std::string& program) {
    auto r = bezel::test::runAotCheckedLenient(program);
    return r.ran;
}
}  // namespace

// The issue's own repro, loop-amplified per LANE policy (a one-shot LSan row
// can pass on both sides — 50-500 iterations is the amplification band).
// `Box<T>`'s `X: T` field instantiated at `string` (`Box<string>`): each
// loop iteration's fresh-string-per-field is invisible to
// isStringStorageMember pre-fix (bare `T`, never `Primitive::String`), so
// `recordHasStringFields(Box)` always returned false and the field's scope-
// end release was never registered — a leak per iteration. Main returns 0;
// under LSan a leak forces a non-zero exit regardless of the program's own
// return value, so `BEZEL_EXPECT_EQ(runChecked(...), 0)` is the FAIL-WITHOUT/
// PASS-WITH oracle.
BEZEL_TEST(generic_record_string_field_leak_3945, bare_type_param_field_at_string_no_leak) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 300 do "
        "    var b := new Box<string>(\"leaktest\" + i.ToString(), 1); "
        "    i += 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// Control: the SAME shape with a NON-GENERIC record — always correctly
// released (isStringStorageMember's bare accessor already saw a concrete
// `string` field regardless of the fix). Pins that the fix did not disturb
// the already-working non-generic path.
BEZEL_TEST(generic_record_string_field_leak_3945, control_non_generic_record_unaffected) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Box(X: string, Y: int); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 300 do "
        "    var b := new Box(\"leaktest\" + i.ToString(), 1); "
        "    i += 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §14.1 / §9.7: TWO fields, the first a bare `T` (the leak's field) and the
// second a concrete `string` — proves the fix's `isStringStorageMember`
// (now `recordMemberTypeResolved`) does not disturb the ALREADY-correct
// concrete-string-field classification, and that BOTH fields are released
// (double the per-iteration allocation footprint of the single-field case,
// still leak-clean).
BEZEL_TEST(generic_record_string_field_leak_3945, mixed_bare_and_concrete_string_fields) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Box<T>(X: T, S: string); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 300 do "
        "    var b := new Box<string>(\"a\" + i.ToString(), \"b\" + i.ToString()); "
        "    i += 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §14.1 / §9.7 / §9.1 struct-layout correctness canary. `recordHasStringFields`
// / `emitRecordStringFieldReleases_impl` now call `recordStruct(ts)` UNDER the
// installed `RecordSubstGuard` — necessary, not decorative: `recordStruct`
// itself branches on the ACTIVE substitution to decide between the record's
// unspecialized (generic-placeholder, `i32`-typed `T`) struct and its
// specialized (concrete-typed `T`) struct (LlvmBackend_Layout.cpp). Without
// the guard active at the RELEASE site, `recordStruct(ts)` returns the WRONG
// (unspecialized) struct type for a GEP whose byte-offset for a trailing
// field can differ from the ACTUAL (specialized) layout the local's alloca
// really has, once a field's specialization width doesn't get absorbed by
// trailing alignment padding — `A: T, B: T` (2 bytes when `T = byte`) ahead
// of `Y: int` (4-byte-aligned) is such a case: unspecialized places `S`
// (string, needs 8-byte alignment) at byte 16 (after two 4-byte `i32`
// placeholders + a 4-byte `int`), specialized places it at byte 8 (after two
// 1-byte fields padded to 4 for `Y`'s alignment, then `Y`, aligned to 8 for
// `S`). MEASURED on a build with the `RecordSubstGuard` install temporarily
// removed from `emitRecordStringFieldReleases_impl` (see the fixing lane's
// verification): this exact shape produced an ASan stack-buffer-overflow
// (an 8-byte OOB read at the wrong GEP-computed offset) at `b`'s scope-end
// release — a MEMORY-CORRUPTION regression strictly worse than the leak this
// fix closes, not merely a missed release. `runAotChecked` both proves the
// VALUES read back correctly (§9.1/§14.1) and that ASan reports nothing on
// disposal.
BEZEL_TEST(generic_record_string_field_leak_3945, struct_layout_matches_at_release_no_corruption) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Box<T>(A: T, B: T, Y: int, S: string); "
        "class C public static method Main(): int "
        "  var b := new Box<byte>(7, 9, 42, \"hello\"); "
        "  if b.A <> 7 then return 1; end; "
        "  if b.B <> 9 then return 2; end; "
        "  if b.Y <> 42 then return 3; end; "
        "  if b.S <> \"hello\" then return 4; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// Plain (non-checked) value sanity for the base repro's field values — the
// checked-oracle tests above cover VALUE correctness too via their `if`
// chains / the base repro's own success path, but this pins the base
// repro's value on the plain runAot oracle as well (belt-and-braces: a
// value regression should fail here even if a future change to the ASan
// harness's own instrumentation masked it there).
BEZEL_TEST(generic_record_string_field_leak_3945, base_repro_value_plain_oracle) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<string>(\"hi\", 1); "
        "  if b.X <> \"hi\" then return 111; end; "
        "  if b.Y <> 1 then return 222; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ===========================================================================
// fix #3964 (§9.7 / §28.9 / §14.1, v0.294): the #3945 REVIEW round found that
// releasing a generic record's bare-type-parameter string field exposed two
// PRE-EXISTING soundness gaps that had been harmless only because nothing
// released such a field before:
//
// (a) RECORD side: isFreshRecordConstructionOfExactType (LlvmBackend_Setup.cpp)
//     is the entire eligibility gate for the #3945 release — it requires
//     every `string`-typed constructor ARGUMENT to be a literal or a proven
//     +1 producer (isPlusOneStringProducer), never a borrow. It read the
//     component's type via sema::storageComponentType — the DECLARED type —
//     so a bare type-parameter component (`X: T`) is Named/no-declaration
//     REGARDLESS of instantiation, and the `string` arm (which enforces the
//     +1-producer requirement) never fired for it. With #3945's release now
//     live, a wrongly-admitted local whose "string" argument was actually a
//     BORROW (a bare identifier read, or a field read) had that borrow
//     released out from under its true owner — heap-use-after-free or a
//     double-release, DEMONSTRABLY WORSE than the leak #3945 fixes.
//
// (b) CLASS side: isStringStorageMember/isClosureStorageMember are SHARED
//     with the CLASS instance-dispose walk (LlvmBackend_Disposal.cpp
//     ~1519/~1548) and its RETAIN half (isStringFieldLvalue →
//     isStringExprType, a SEMA-level check that can never see through a
//     monomorphization). An earlier revision of this fix resolved
//     isStringStorageMember itself (not a separate twin), which broke that
//     retain/release BALANCE for a generic class's bare-type-parameter
//     string field: dispose started releasing a +1 the (still-blind) retain
//     half never minted.
//
// THE FIX: isFreshRecordConstructionOfExactType now resolves a bare
// component through the construction's OWN genericArgs, failing CLOSED
// (declining the fast-path entirely) when unresolvable — never assuming
// eligibility it cannot prove. isStringStorageMember/isClosureStorageMember
// are reverted to bare (restoring the CLASS side's original, consistently-
// blind balance); a new isStringStorageMemberResolved twin is used ONLY by
// the RECORD walk (recordHasStringFields / emitRecordStringFieldReleases_impl),
// which is now SAFE specifically because of fix (a)'s eligibility tightening.
//
// MEASURED (fixing lane, `bezelc --sanitize=address` on each generated
// program, 100-200 loop iterations, differential against a freshly built
// parent at 1e5045e — reproduced independently, not just trusted from the
// review): all five of the review's regressions are gone and every #3945
// gain is retained:
//   df2_outlives                    parent: 8690 B leak   pre-#3964: HEAP-UAF   post-#3964: 8646 B leak (matches non-generic control exactly)
//   df4_two_records_one_string      parent: 4841 B leak   pre-#3964: SEGV(free) post-#3964: 4890 B leak
//   df5_field_read_arg              parent: 4190 B leak   pre-#3964: HEAP-UAF   post-#3964: CLEAN (copy.X aliases src.X's own pointer; nothing separate to leak)
//   cls1_generic_class_string_field parent: clean         pre-#3964: HEAP-UAF   post-#3964: clean
//   cls2_generic_class_plusone      parent: clean         pre-#3964: SEGV(free) post-#3964: clean
// Non-generic controls (df2b, cls3) are BYTE-IDENTICAL to their generic
// counterparts post-#3964, on both sides of the fix — confirming the
// defect (and the fix) is generic-only.
// ===========================================================================

// df2: a fresh `+1` string `s` is passed as a BORROW argument (a bare
// identifier read, not a producer) into a generic record's bare-`T` field,
// then `s` itself is read again afterward (still alive, still owned by its
// own local). Pre-#3964 (with only #3945's release live): the wrongly-
// admitted `b` released `s`'s reference at its own scope end, and the LATER
// read of `s` was a heap-use-after-free. Post-#3964: `isFreshRecordConstructionOfExactType`
// correctly declines eligibility (the argument is a borrow, not a literal or
// +1 producer per §28.3), so `b`'s field is never registered for release —
// falling to the safe (leak) direction instead, identical to the non-generic
// control. `didNotCrash` proves no UAF; the plain-oracle value assertion
// proves the computed VALUE (acc mod 100) is still correct.
BEZEL_TEST(generic_record_string_field_leak_3945, df2_outlives_borrow_argument_no_uaf) {
    const char* prog =
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 200 do "
        "    var s := \"abcdefgh\" + i.ToString(); "
        "    if i >= 0 then "
        "      var b := new Box<string>(s, 1); "
        "      acc += b.Y; "
        "    end; "
        "    acc += s.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    BEZEL_EXPECT_EQ(run(prog), 90);
}

// Control: the IDENTICAL shape with a non-generic record. Byte-identical
// leak/value behavior to df2 above both before and after #3964 — pins that
// the defect (and the fix) is generic-only, not a change to the non-generic
// path.
BEZEL_TEST(generic_record_string_field_leak_3945, df2_control_non_generic_matches) {
    const char* prog =
        "module M; record Box(X: string, Y: int); "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 200 do "
        "    var s := \"abcdefgh\" + i.ToString(); "
        "    if i >= 0 then "
        "      var b := new Box(s, 1); "
        "      acc += b.Y; "
        "    end; "
        "    acc += s.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    BEZEL_EXPECT_EQ(run(prog), 90);
}

// df4: ONE string `s` is passed as a borrow argument into TWO separate
// generic-record locals (`b1`, `b2`). Pre-#3964: BOTH were wrongly admitted
// as sole owners, so BOTH released `s` at scope end — a double-release
// (SEGV inside `free()`, refcount underflow). Post-#3964: neither is
// admitted (the argument is a borrow), so neither releases — no double-free,
// falls to the safe leak direction.
BEZEL_TEST(generic_record_string_field_leak_3945, df4_two_records_share_one_borrow_no_double_free) {
    const char* prog =
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    var s := \"shared-string-\" + i.ToString(); "
        "    if i >= 0 then "
        "      var b1 := new Box<string>(s, 1); "
        "      var b2 := new Box<string>(s, 2); "
        "      acc += b1.Y + b2.Y; "
        "    end; "
        "    acc += s.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    BEZEL_EXPECT_EQ(run(prog), 90);
}

// df5: a generic record's OWN field (`src.X`, a fresh +1 producer bound at
// `src`'s construction) is read and passed as a borrow argument into a
// SECOND generic-record local (`copy`). Pre-#3964: `copy` was wrongly
// admitted, so its scope-end release freed the SAME string `src` still owns
// (read again via `src.X.Length` afterward) — heap-use-after-free.
// Post-#3964: `copy`'s field is correctly declined (the argument is a field
// read, not a producer) — and since `copy.X` is a bare pointer COPY of
// `src.X` (not a separate allocation), not releasing it introduces no NEW
// leak either: this cell is fully clean, not just crash-free.
BEZEL_TEST(generic_record_string_field_leak_3945, df5_field_read_as_ctor_arg_no_uaf) {
    const char* prog =
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    var src := new Box<string>(\"origin-\" + i.ToString(), 1); "
        "    if i >= 0 then "
        "      var copy := new Box<string>(src.X, 2); "
        "      acc += copy.Y; "
        "    end; "
        "    acc += src.X.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 90);  // clean: no leak AND no crash
    BEZEL_EXPECT_EQ(run(prog), 90);
}

// cls1: a GENERIC CLASS (not a record) with a bare-type-parameter instance
// field (`_val: T`) set via ordinary constructor-body assignment
// (`_val := v;`). Pre-#3964 (isStringStorageMember itself resolved, no
// separate twin): the SHARED class-dispose walk started releasing `_val`
// (now recognized as string-typed) on `Dispose()`, but the RETAIN half
// (isStringFieldLvalue, sema-level, blind to the substitution) never minted
// that +1 at the constructor assignment — releasing a reference the class
// instance never owned, a heap-use-after-free on the caller's own `s`
// (still read via `s.Length` afterward). Post-#3964: isStringStorageMember
// is back to bare, so the class dispose walk stays exactly as blind as the
// (also-blind) retain half — consistently balanced, at the cost of a
// pre-existing, UNDISTURBED leak of `_val` (not this fix's to close).
BEZEL_TEST(generic_record_string_field_leak_3945, cls1_generic_class_string_field_no_uaf) {
    const char* prog =
        "module M; "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method Len(): int return 1; end Len; "
        "  destructor end destructor; "
        "end Container; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    var s := \"class-held-\" + i.ToString(); "
        "    if i >= 0 then "
        "      using var c := new Container<string>(s); "
        "      acc += c.Len(); "
        "    end; "
        "    acc += s.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    BEZEL_EXPECT_EQ(run(prog), 90);
}

// Control: the IDENTICAL class shape, non-generic (`_val: string` directly).
// Byte-identical behavior to cls1 both before and after #3964.
BEZEL_TEST(generic_record_string_field_leak_3945, cls1_control_non_generic_class_matches) {
    const char* prog =
        "module M; "
        "class Container "
        "  protected _val: string; "
        "  public constructor(v: string) _val := v; end constructor; "
        "  public method Len(): int return 1; end Len; "
        "  destructor end destructor; "
        "end Container; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    var s := \"class-held-\" + i.ToString(); "
        "    if i >= 0 then "
        "      using var c := new Container(s); "
        "      acc += c.Len(); "
        "    end; "
        "    acc += s.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    BEZEL_EXPECT_EQ(run(prog), 90);
}

// cls2: the most ordinary generic-class-holding-a-string program there is —
// a fresh `+1` string producer supplied DIRECTLY as the constructor
// argument (no intervening local to alias). Pre-#3964 (isStringStorageMember
// itself resolved): SEGV inside `free()` — the same retain/release
// imbalance cls1 exposes as a use-after-free instead, the exact crash
// signature depending on this shape's own store/retain sequencing, not
// independently traced further here. Post-#3964: fully clean — no leak, no
// crash. This shape was CLEAN ON THE PARENT (before #3945 existed at all);
// it is the starkest evidence the review's "you under-claimed" verdict was
// right.
BEZEL_TEST(generic_record_string_field_leak_3945, cls2_generic_class_plusone_no_double_free) {
    const char* prog =
        "module M; "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method Len(): int return 1; end Len; "
        "  destructor end destructor; "
        "end Container; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    using var c := new Container<string>(\"class-held-\" + i.ToString()); "
        "    acc += c.Len(); "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 0);  // clean on both parent and post-fix
    BEZEL_EXPECT_EQ(run(prog), 0);
}

// Stronger struct-layout canaries (per review): the SAME
// struct_layout_matches_at_release_no_corruption shape at T = long and
// T = string — native, ordinary instantiations (not the contrived `byte`
// case above) that ALREADY SEGV'd in bz_string_release ON THE PARENT
// (before #3944/#3945 existed at all): `recordStruct(ts)` called with no
// active substitution at disposal time returned the unspecialized (i32-
// placeholder) struct, computing the WRONG byte offset for `S` whenever a
// preceding bare-`T` field's specialized width isn't absorbed by trailing
// alignment padding (both `long` (i64) and `string` (ptr) are 8 bytes,
// vs. the i32 placeholder's 4). #3945's RecordSubstGuard install inside
// emitRecordStringFieldReleases_impl fixes this PRE-EXISTING corruption as
// a byproduct, not just the byte-instantiation case.
BEZEL_TEST(generic_record_string_field_leak_3945, struct_layout_canary_long_no_corruption) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Box<T>(A: T, B: T, Y: int, S: string); "
        "class C public static method Main(): int "
        "  var b := new Box<long>(7, 9, 42, \"hello\"); "
        "  if b.A <> 7 then return 1; end; "
        "  if b.B <> 9 then return 2; end; "
        "  if b.Y <> 42 then return 3; end; "
        "  if b.S <> \"hello\" then return 4; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}
BEZEL_TEST(generic_record_string_field_leak_3945, struct_layout_canary_string_no_corruption) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Box<T>(A: T, B: T, Y: int, S: string); "
        "class C public static method Main(): int "
        "  var b := new Box<string>(\"aa\", \"bb\", 42, \"hello\"); "
        "  if b.A <> \"aa\" then return 1; end; "
        "  if b.B <> \"bb\" then return 2; end; "
        "  if b.Y <> 42 then return 3; end; "
        "  if b.S <> \"hello\" then return 4; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// Positive control (per review): a record whose string argument genuinely
// IS a `+1` producer (a string-concatenation expression, never a borrow) —
// proves isFreshRecordConstructionOfExactType's tightened bare-type-param
// resolution does not OVER-reject a sound construction. Must stay leak-clean
// (the #3945 gain this whole family exists to protect). 200 iterations each
// contributing `b.Y == 1`: acc == 200, acc mod 100 == 0.
BEZEL_TEST(generic_record_string_field_leak_3945, genuine_plusone_argument_still_eligible) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 200 do "
        "    var b := new Box<string>(\"abcdefgh\" + i.ToString(), 1); "
        "    acc += b.Y; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;"), 0);
}
