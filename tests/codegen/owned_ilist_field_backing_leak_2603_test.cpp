// fix #2603 (codegen leak, §28.9 / §28.11 / §15.4 / §4.1, v0.236): an `owned`
// field whose STATIC type is `IList<T>` (the borrowing list interface) leaks
// its `bz_array_t*` backing storage at scope end.
//
// ROOT CAUSE: `Emitter::ownedCollectionFieldType` (LlvmBackend_impl.hpp ~6427)
// classifies an `owned` field as collection-typed by consulting
// `isCollectionTypeName` / `isLinkedListTypeName` / `isDictTypeName`, ALL of
// which check CONCRETE builtin names (Array/Stack/Queue/.../LinkedList/...
// /Dictionary/...). An `IList<T>` field's head name is "IList" — not in any of
// those registries — so `ownedCollectionFieldType` returns nullptr, the field
// never reaches `emitOwnedFieldDisposals`'s collection path, and its backing
// (from `bz_array_alloc`) is never freed when the holder is disposed.
//
// This is the field-disposal analogue of issue #401, which established that an
// IList<T> receiver is ARRAY-BACKED — it carries the SAME `bz_array_t*` as
// Array<T> and lowers through the IDENTICAL `bz_array_*` runtime. Per #401
// (LlvmBackend_Calls.cpp:429-446 `isListInterfaceName`): IList<T> /
// IReadOnlyList<T> are borrowing + array-backed; IOwningList<T> is owning. The
// field-disposal path missed that widening, so an `owned IList<T>` field —
// which holds an Array<T> backing — leaked it.
//
// FIX: widen `ownedCollectionFieldType` to admit the BORROWING list interfaces
// (IList<T> / IReadOnlyList<T>) via a new `isBorrowingListInterfaceName`
// helper, so the field routes through `emitOwnedFieldDisposals` /
// `lowerOwnedCollectionFieldStore` and frees its backing via `bz_array_free`
// (the array-backed default), with the element disposer from elementPolicyFor
// (stringElementReleaseThunk for a string element). IOwningList<T> (owning) is
// EXCLUDED — `isOwningPrefixedName("IOwningList")` is false, so admitting it
// would misclassify its elements as borrowing and leak them; filed separately.
// The two downstream string-element-release fallbacks (Disposal.cpp:615 scope-
// end disposal, Setup.cpp:15612 drop-on-overwrite) are widened to admit IList
// / IReadOnlyList alongside Array/Stack/Queue, mirroring the #2598 indexer fix.
//
// OBSERVABILITY: the leak is a DIRECT leak of `bz_array_t*` headers + their
// byte buffers, so runAotChecked's LSan flags it. Pre-fix `r.ran` is false
// ("Direct leak of N byte(s)"), post-fix the run is clean and the exit code
// independently confirms the program computed correctly (no double-free — a
// double-free of the backing would crash under ASan and flip r.ran false).
//
// REGRESSION COVERAGE:
//   * (A) CARDINAL — owned IList<string> field, the issue's repro. Each of 100
//     iterations constructs a Holder whose `owned _arr: IList<string>` field is
//     allocated (bz_array_alloc), populated, and dropped at scope end. Pre-fix
//     LSan: 16800 byte(s) / 200 allocations (100 headers + 100 byte buffers).
//     Post-fix: LSan-clean, exit 44 (sink 300 mod 256).
//   * (B) DYNAMIC STRINGS — owned IList<string> field populated with ToString
//     results. Confirms the string ELEMENTS (each +1) are released at free, not
//     just the backing — exercises the widened string-element-release fallback
//     at Disposal.cpp:615. Post-fix: LSan-clean, exit 244 (500 mod 256).
//   * (C) REASSIGNMENT — owned IList<string> field REASSIGNED (_arr := new
//     Array<string>()), exercising the drop-on-overwrite path
//     (lowerOwnedCollectionFieldStore, Setup.cpp:15612). The OLD backing is
//     freed and OLD string elements released. Post-fix: LSan-clean, exit 94.
//   * (D) owned Array<string> FIELD CONTROL — the concrete borrowing array. Must
//     stay clean (no regression in the existing Array path). exit 44.
//   * (E) owned IList<int> FIELD CONTROL — int elements (non-string). The
//     widened string-element gate must NOT admit IList<int>; the generic
//     bz_array_free(path, null-disposer) path handles value elements. Guards
//     against the string-element thunk misfiring on int data. exit 144.
//   * (F) owned IReadOnlyList<string> FIELD — the read-only borrowing twin
//     (same array-backed ABI per #401, read-only so it cannot be populated via
//     the interface; the initializer's empty Array backing is what leaks). The
//     widening covers it alongside IList. Post-fix: LSan-clean, exit 0.
//
// SPEC CITATION: §28.9 (runtime-managed string refcounting), §28.11 (collection
// element ownership — borrowing family co-owns a +1 per runtime-managed
// element), §15.4 (collection literals cannot target an interface type), §4.1
// (IList<T>/IReadOnlyList<T> are array-backed, same bz_array_t* as Array<T> per
// issue #401). §28.5 (owned field disposal cascades into the holder's Dispose).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ----------------------------------------------------------------------
// Probe A — CARDINAL: owned IList<string> field backing-storage leak (the
// issue's repro). 100 iterations; each constructs a Holder (whose `owned
// _arr: IList<string>` field is allocated via bz_array_alloc), populates + sets
// the field, and drops it at scope end. Pre-fix LSan: 16800 byte(s) / 200
// allocations (100 bz_array_t* headers + 100 indirect byte buffers). Post-fix:
// LSan-clean, exit 44 (sink = 100 × 3 = 300 mod 256).
// ----------------------------------------------------------------------
const char* kOwnedIListStringField =
    "module M; "
    "class Holder "
    "  private owned _arr: IList<string> := new Array<string>(); "
    "  public method AddIt(): int "
    "    _arr.Add(\"abc\"); "
    "    _arr[0] := \"xyz\"; "
    "    return _arr[0].Length; "  // 3
    "  end AddIt; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      using var h := new Holder(); "
    "      sink := sink + h.AddIt(); "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 300 mod 256 = 44
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe B — DYNAMIC STRINGS: owned IList<string> field populated with
// dynamically-allocated strings (interpolation). Each +1 string element must be
// released at free via the widened string-element-release fallback
// (Disposal.cpp:615). Pre-fix the backing leak dominates; post-fix the elements
// are also confirmed released. Post-fix: LSan-clean, exit 244 (500 mod 256).
// ----------------------------------------------------------------------
const char* kOwnedIListStringDynamic =
    "module M; "
    "class Holder "
    "  private owned _arr: IList<string> := new Array<string>(); "
    "  public method Fill(n: int): int "
    "    var i := 0; "
    "    while i < n do "
    "      _arr.Add($\"s_{i}\"); "
    "      i := i + 1; "
    "    end; "
    "    return _arr.Count; "  // n
    "  end Fill; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      using var h := new Holder(); "
    "      sink := sink + h.Fill(5); "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 500 mod 256 = 244
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe C — REASSIGNMENT: owned IList<string> field reassigned
// (`_arr := new Array<string>()`), exercising the drop-on-overwrite path
// (lowerOwnedCollectionFieldStore, Setup.cpp:15612). The OLD backing is freed
// and the OLD string elements released. Post-fix: LSan-clean, exit 94
// (50 × 7 = 350 mod 256).
// ----------------------------------------------------------------------
const char* kOwnedIListStringReassign =
    "module M; "
    "class Holder "
    "  private owned _arr: IList<string> := new Array<string>(); "
    "  public method FillAndReset(): int "
    "    _arr.Add($\"item_{1}\"); "
    "    _arr.Add($\"item_{2}\"); "
    "    var oldLen := _arr[0].Length; "  // \"item_1\" = 6
    "    _arr := new Array<string>(); "   // drop-on-overwrite: free old + 2 strs
    "    _arr.Add($\"fresh_{oldLen}\"); " // \"fresh_6\" = 7
    "    return _arr[0].Length; "         // 7
    "  end FillAndReset; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var i := 0; var sink := 0; "
    "    while i < 50 do "
    "      sink := sink + h.FillAndReset(); "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 350 mod 256 = 94
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe D — owned Array<string> FIELD CONTROL: the concrete borrowing array
// field. The widening must NOT regress the existing Array path. Pre/post-fix
// LSan-clean, exit 44 (300 mod 256).
// ----------------------------------------------------------------------
const char* kOwnedArrayStringField =
    "module M; "
    "class Holder "
    "  private owned _arr: Array<string> := new Array<string>(); "
    "  public method AddIt(): int "
    "    _arr.Add(\"abc\"); "
    "    _arr[0] := \"xyz\"; "
    "    return _arr[0].Length; "  // 3
    "  end AddIt; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      using var h := new Holder(); "
    "      sink := sink + h.AddIt(); "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 300 mod 256 = 44
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe E — owned IList<int> FIELD CONTROL: int elements (non-string). The
// widened string-element-release gate must NOT admit IList<int> (the string-
// primitive guard excludes int), so the generic bz_array_free(path, null)
// path handles value elements. Guards against the string-element thunk
// misfiring on int data (which would crash). exit 144 (400 mod 256).
// ----------------------------------------------------------------------
const char* kOwnedIListIntField =
    "module M; "
    "class Holder "
    "  private owned _arr: IList<int> := new Array<int>(); "
    "  public method Fill(n: int): int "
    "    var i := 0; "
    "    while i < n do "
    "      _arr.Add(i); "
    "      i := i + 1; "
    "    end; "
    "    return _arr.Count; "  // n
    "  end Fill; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      using var h := new Holder(); "
    "      sink := sink + h.Fill(4); "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 400 mod 256 = 144
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe F — owned IReadOnlyList<string> FIELD: the read-only borrowing twin.
// Per #401 IReadOnlyList<T> is array-backed (same bz_array_t* as Array<T>),
// read-only so it cannot be populated through the interface; the initializer's
// empty Array backing is what leaks pre-fix. The widening covers it alongside
// IList. Pre-fix LSan: 16800 byte(s) / 200 allocations. Post-fix: LSan-clean,
// exit 0.
// ----------------------------------------------------------------------
const char* kOwnedIReadOnlyListStringField =
    "module M; "
    "class Holder "
    "  private owned _arr: IReadOnlyList<string> := new Array<string>(); "
    "  public method Nop(): int "
    "    return 0; "
    "  end Nop; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      using var h := new Holder(); "
    "      sink := sink + h.Nop(); "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 0
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2603 — CARDINAL: owned IList<string> field backing leak. Pre-fix LSan
// flags 16800 byte(s) / 200 allocations. Post-fix the field's bz_array_t*
// backing is freed via bz_array_free at the holder's Dispose.
// LSan-clean + value 44.
// =====================================================================
BEZEL_TEST(codegen_owned_ilist_field_backing_leak_2603,
           owned_ilist_string_field_backing_freed_spec_28_9_28_11) {
    JitResult r = runAotChecked(kOwnedIListStringField);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);           // LSan-clean (the #2603 leak flips this)
    BEZEL_EXPECT_EQ(r.exitCode, 44);    // 300 mod 256 — independent value check
}

// =====================================================================
// #2603 — DYNAMIC STRINGS: the string ELEMENTS (each +1 from interpolation)
// are released at free via the widened string-element-release fallback. Pre-fix
// the backing leak dominates (16800 B); post-fix both backing and elements are
// confirmed released. LSan-clean + value 244.
// =====================================================================
BEZEL_TEST(codegen_owned_ilist_field_backing_leak_2603,
           owned_ilist_string_field_elements_released_spec_28_9_28_11) {
    JitResult r = runAotChecked(kOwnedIListStringDynamic);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 244);   // 500 mod 256
}

// =====================================================================
// #2603 — REASSIGNMENT: the drop-on-overwrite path
// (lowerOwnedCollectionFieldStore) frees the OLD backing and releases the OLD
// string elements when an owned IList<string> field is reassigned. Exercises
// the widened string-element-release fallback at Setup.cpp:15612.
// LSan-clean + value 94.
// =====================================================================
BEZEL_TEST(codegen_owned_ilist_field_backing_leak_2603,
           owned_ilist_string_field_reassign_frees_old_spec_28_9_28_11) {
    JitResult r = runAotChecked(kOwnedIListStringReassign);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 94);    // 350 mod 256
}

// =====================================================================
// #2603 — owned Array<string> FIELD regression guard: the predicate widening
// must not regress the existing concrete-Array field-disposal path.
// LSan-clean + value 44.
// =====================================================================
BEZEL_TEST(codegen_owned_ilist_field_backing_leak_2603,
           owned_array_string_field_remains_clean_spec_28_9_28_11) {
    JitResult r = runAotChecked(kOwnedArrayStringField);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);    // 300 mod 256
}

// =====================================================================
// #2603 — owned IList<int> FIELD control: int elements (non-string). The
// widened string-element gate excludes int, so the generic value-element path
// handles disposal (bz_array_free with a null element disposer). Guards against
// the string-element thunk misfiring on int data (would crash).
// LSan-clean + value 144.
// =====================================================================
BEZEL_TEST(codegen_owned_ilist_field_backing_leak_2603,
           owned_ilist_int_field_unaffected_spec_28_9_28_11) {
    JitResult r = runAotChecked(kOwnedIListIntField);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 144);   // 400 mod 256
}

// =====================================================================
// #2603 — owned IReadOnlyList<string> FIELD: the read-only borrowing twin
// (array-backed per #401). The initializer's empty Array backing leaks pre-fix
// (16800 B / 200 allocs); the widening covers it alongside IList.
// LSan-clean + value 0.
// =====================================================================
BEZEL_TEST(codegen_owned_ilist_field_backing_leak_2603,
           owned_ireadonlylist_string_field_backing_freed_spec_28_9_28_11) {
    JitResult r = runAotChecked(kOwnedIReadOnlyListStringField);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
