// Regression tests for #2738 — codegen leak: `yield return` of a freshly
// minted string leaks every element from §29.2 row 3's "shared reference"
// assumption. §28.3 (producers, v0.184) / §29.2 (v0.155) (v0.282).
//
// THE BUG. §29.2 row 3 (string / runtime-managed element) says "the
// enumerator does not dispose anything for these yield types; the next
// MoveNext simply produces the next value." Pre-fix this was taken literally:
// the iterator's current_val slot held the +1 reference minted by
// `yield return <+1 producer>` (interpolation, `+`, `??`, case, conditional,
// range slice, ToString — `isPlusOneStringProducer`), and NO release ever
// fired for that +1, neither at the next yield's store (which silently
// overwrote the slot) nor at iterator Dispose (which early-returned for the
// non-class non-owning shape). Each `yield return` of a +1 producer thus
// orphaned one refcount-1 string at scope exit — for the issue's repro a
// 3-iteration `Gen(): IEnumerable<string>` yielding `$"g-{i}"` over ToArray()
// leaked 108 bytes / 6 allocations (3 strings × {header, data} = 36 bytes
// each on the issue's target).
//
// THE FIX (codegen, three coordinated sites).
//   (1) LlvmBackend_Stmts.cpp yield handler (v0.282): a new gate mirroring
//       the class-element prev-dispose (#1120) — for the runtime-managed
//       primitive element kind (non-nullable `string`), the YieldStmt's
//       store into current_val now disposes the PREVIOUS current_val if the
//       owned-flag byte indicates the iterator owned the +1, and updates the
//       owned flag based on `isPlusOneStringProducer(ys->value)`. Disjoint
//       from the class-element gate (this fires for primitives) and the
//       owning-iter gate (this fires for the borrowing head) — the three are
//       exhaustive and disjoint by construction in emitIteratorMethod.
//   (2) LlvmBackend_Iterators.cpp `disposeYieldedVal` lambda: extends the
//       early-return gate to also fire for runtime-managed primitives, and
//       in the non-null branch emits `emitStringRelease` (the refcounted
//       string release path) instead of `emitDisposeAndReclaim` (the class
//       vtable dispatch path). Reuses the existing CURRENT_VAL_OWNED_FIELD
//       byte — both class (#1120) and string (#2738) element kinds share
//       the same flag, keyed on different predicates for "did the yield
//       mint a fresh +1?".
//   (3) LlvmBackend_Calls3.cpp materialize drain (the §29.11
//       materializeVtableIEnumerableToArray bridge for vtable-backed
//       IEnumerable<T> receivers): retains each curVal pointer before
//       appending to the materialize's bz_array_t — so the iterator's
//       yield-store prev-dispose and Dispose can release their +1 copies
//       without leaving dangling pointers in mat_arr. The outer
//       bz_array_retain_strings at the LINQ-ToArray intercept is
//       NARROWLY skipped for this same shape (`isVtableBackedIEnumerableType`
//       AND non-nullable string primitive) so the mat_arr's per-element
//       retains are the SOLE ownership transfer — the outer retain would
//       over-count by 1 per element (mat_arr is freed at bz_lazy_dispose
//       before result-array scope end, orphaning its retains → leak). For
//       non-vtable-backed sources (Array<T>, chain ending in Array<T> /
//       Lazy<T>) the outer retain stays in place; the source's lifetime
//       matches the result's and the +1 balances symmetrically.
//
// ORACLE. `runAotChecked` (compile + native exec, AddressSanitizer +
// LeakSanitizer, fork-isolated). A leak forces a nonzero exit on its own
// (`opts.sanitizeAddress = true` links the generated program with ASan+LSan
// — `Support/aot_support.hpp:42` S3). `r.ran` is false if the binary
// crashed (ASan fire) or the compile failed; otherwise the program's exit
// code is returned. Each program returns a value derived from the iterated
// elements (length sum / content equality) so the assertion is on BOTH the
// correct program output AND a leak-clean exit — the two failure
// directions are distinguished by `r.ran` (crash vs. clean exit) and the
// returned value (correct sum vs. wrong sum / sentinel).
//
// NARROW. The fix only fires on the borrowing head (`!iterIsOwningIter_`)
// with a non-nullable string element — `IOwningEnumerable<string>` keeps
// its TakeCurrent-based transfer (#1349), nullable-string / value-primitive
// / class-element iterators keep their existing handling, and the LINQ
// chain terminal nodes (Where / Select / …) compose uniformly over the
// vtable-backed materialize path.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Sentinel: -999 when the AOT run fails (compile failure or ASan / LSan
// fire flips `ran` to false), the program's exit code otherwise. Each
// program returns an integer derived from the iterated elements, so
// `r.ran == true && r.exitCode == <expected>` is the load-bearing
// assertion (correct program behavior + clean exit).
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ============================================================================
// d1 — THE ISSUE'S HEADLINE REPRO. §28.3 / §29.2: `Gen(i0)` yields three
// freshly minted interpolation strings (parameter-driven to defeat the
// constant-fold that turns `$"g-{1}"` etc. into a sentinel literal,
// making the +1 producer a runtime interpolation); consumer materializes
// via `.ToArray()` into a using-bound `Array<string>`. Pre-fix the
// materialize path's arr ends up with pointers whose +1 references were
// orphaned at the iterator (the §29.2 row 3 assumption), and the outer
// bz_array_retain_strings over-counts because mat_arr's pointer copies
// already held the references — net leak of 3 strings × 36 bytes = 108
// bytes / 6 allocations. Post-fix the materialize retains each curVal
// (owning transfer), the iterator's yield-store prev-dispose and Dispose
// release their +1 copies, the outer retain is skipped for this
// vtable-backed shape, and result.Dispose releases the SOLE ownership →
// rc 0 → no leak. Length 3 + content equality.
//
// The materialized `Array<string>` is read through `.Count`, not `.Length`:
// stdlib §4.4's Count-vs-Length convention gives a resizable collection
// `Count` and reserves `Length` for a fixed or contiguous extent, so
// `Array<T>.Length` is rejected ("no member `Length` on `Array<T>`: use
// `Count`"). The `s.Length` reads in the rows below are a different
// receiver — a `string` element — and `Length` is correct there.
// ============================================================================
BEZEL_TEST(yield_return_runtime_managed_leak_2738,
           toarray_of_fresh_interpolation_strings_leak_free_29_2) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Gen(i0: int): IEnumerable<string> "
        "    yield return $\"g-{i0}\"; "
        "    yield return $\"g-{i0 + 1}\"; "
        "    yield return $\"g-{i0 + 2}\"; "
        "  end Gen; "
        "  public static method Main(): int "
        "    do "
        "      using var t: Array<string> := Gen(1).ToArray(); "
        "      if t.Count <> 3 then return 1; end; "  // Array<T> says Count (stdlib §4.4)
        "      if t[0] <> \"g-1\" then return 2; end; "
        "      if t[1] <> \"g-2\" then return 3; end; "
        "      if t[2] <> \"g-3\" then return 4; end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ============================================================================
// d2 — for-in consumer variant. §28.3 / §29.2: the same Gen(i0) iterated
// by `for var s in src do … end` — the for-in protocol does NOT take the
// +1 (the for-var is a borrow, not a consumer — see seedOwnedIterVar /
// Ownership.cpp:2434 which only fires for IOwningEnumerable /
// IOwningAsyncEnumerable). The iterator's current_val slot is the SOLE
// holder of the +1 across each iteration; pre-fix the leak spans every
// yielded string (no Dispose-time release, no yield-store prev-dispose).
// Post-fix the yield-store prev-dispose releases each +1 BEFORE the next
// yield stores a new value (no overlap), and the iterator's Dispose
// releases the outstanding +1 — exactly-once. Sum of "g-1".Length +
// "g-2".Length + "g-3".Length = 3 + 3 + 3 = 9.
// ============================================================================
BEZEL_TEST(yield_return_runtime_managed_leak_2738,
           for_in_of_fresh_interpolation_strings_leak_free_29_2) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Gen(i0: int): IEnumerable<string> "
        "    yield return $\"g-{i0}\"; "
        "    yield return $\"g-{i0 + 1}\"; "
        "    yield return $\"g-{i0 + 2}\"; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var s in Gen(1) do sum := sum + s.Length; end; "
        "    if sum <> 9 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ============================================================================
// d3 — manual enumerator consumer. §29.1: `using var en := Gen().GetEnumerator();
// while en.MoveNext() do use en.Current end` exercises the same fix path
// via the manual GetEnumerator protocol (no desugaring surprise). Same
// exactly-once invariant: content match. "g-10g-20g-30" concatenates the
// three yielded strings.
// ============================================================================
BEZEL_TEST(yield_return_runtime_managed_leak_2738,
           manual_enumerator_fresh_interpolation_strings_leak_free_29_2) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Gen(i0: int): IEnumerable<string> "
        "    yield return $\"g-{i0}\"; "
        "    yield return $\"g-{i0 + 10}\"; "
        "    yield return $\"g-{i0 + 20}\"; "
        "  end Gen; "
        "  public static method Main(): int "
        "    do "
        "      using var en := Gen(10).GetEnumerator(); "
        "      var seen := \"\"; "
        "      while en.MoveNext() do seen := seen + en.Current; end; "
        "      if seen <> \"g-10g-20g-30\" then return 1; end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ============================================================================
// d4 — control: LITERAL `yield return` of a static-sentinel string is NOT
// a +1 producer. isPlusOneStringProducer rejects StringLiteralExpr
// (immortal sentinel, rc=∞), so the owned flag stays 0 and the iterator's
// prev-dispose / Dispose both NO-OP on this slot. No retention, no
// release — same as pre-fix behavior for this shape; this test pins the
// NARROW by confirming the fix doesn't gratuitously retain/release
// literal-yield iterators. Sum 18 (6+6+6).
// ============================================================================
BEZEL_TEST(yield_return_runtime_managed_leak_2738,
           literal_yield_string_not_a_plus_one_producer_no_retention) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<string> "
        "    yield return \"literal-1\"; "
        "    yield return \"literal-2\"; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var s in Gen() do n := n + s.Length; end; "
        "    if n <> 18 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ============================================================================
// d5 — control: NON-NULLABLE string yield that's a BORROW of a local
// binding (`var b := $\"g-{i0}\"; yield return b;`) is NOT a +1 producer
// at the yield site — the local owns the +1 and the iterator sees a
// re-borrow. The owned flag stays 0; the iterator's yield-store
// prev-dispose and Dispose both NO-OP on this slot; the local's scope-
// end release is the sole releaser. This pins the
// `!isPlusOneStringProducer(<local>)` arm of the predicate and confirms
// the fix doesn't double-release a borrowed +1.
// ============================================================================
BEZEL_TEST(yield_return_runtime_managed_leak_2738,
           borrowed_local_yield_does_not_double_release) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Gen(i0: int): IEnumerable<string> "
        "    var b := $\"g-{i0}\"; "
        "    yield return b; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var seen := \"\"; "
        "    for var s in Gen(99) do seen := seen + s; end; "
        "    if seen <> \"g-99\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ============================================================================
// d6 — control: ARRAY<T>.ToArray() (the inline-array path) is unaffected
// by the vtable-backed outer-retain skip — the source array holds the
// +1 refs for its strings for the result's lifetime, and the outer
// retain remains. The ToArray returns a snapshot, the source's
// using-scope release pairs with the result's release. Length 3, content
// order preserved (`t.Count` — an `Array<T>` receiver, stdlib §4.4).
// ============================================================================
BEZEL_TEST(yield_return_runtime_managed_leak_2738,
           array_to_array_outer_retain_still_applies_602_control) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var src: Array<string> := new Array<string>(); "
        "      src.Add(\"hello\"); "
        "      src.Add(\"world\"); "
        "      src.Add(\"foo\"); "
        "      using var t: Array<string> := src.ToArray(); "
        "      if t.Count <> 3 then return 1; end; "  // Array<T> says Count (stdlib §4.4)
        "      if t[0] <> \"hello\" then return 2; end; "
        "      if t[1] <> \"world\" then return 3; end; "
        "      if t[2] <> \"foo\" then return 4; end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ============================================================================
// d7 — control: a CLASS-element iterator (IEnumerable<MyClass>) — the
// existing #1120 prev-dispose + Dispose path keeps working unchanged. The
// class-element gate at LlvmBackend_Stmts.cpp and the `elemIsOwnedClass`
// branch of `disposeYieldedVal` are DISJOINT from this fix's runtime-
// managed primitive gate (the predicates are mutually exclusive at
// emitIteratorMethod time — class/interface kind vs primitive string
// kind). ToArray over `IEnumerable<MyClass>` should still work end-to-end.
// Each MyClass instance is constructed with a fresh id; the destructor
// records a release count on a static field. The leak-free invariant is
// that `_r == _n` after the loop. We use a NON-empty-class element here
// (rather than the #1120 stock Cell) only to keep the test self-contained;
// the assertion shape is identical to iter_yield_owned_dispose_1120.
// ============================================================================
BEZEL_TEST(yield_return_runtime_managed_leak_2738,
           class_element_iterator_unaffected_by_runtime_managed_gate_1120_control) {
    const std::string src =
        "module M; "
        "class Box "
        "  public id: int; "
        "  public constructor(x: int) id := x; end constructor; "
        "end Box; "
        "class C "
        "  public static method Gen(i0: int): IEnumerable<Box> "
        "    yield return new Box(i0); "
        "    yield return new Box(i0 + 1); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var b in Gen(10) do sum := sum + b.id; end; "
        "    if sum <> 21 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}