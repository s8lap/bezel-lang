// Codegen — issue #2778 / §28.6 / §23 / §29.7: storing a collection into an
// `IEnumerable<T>`-typed CLASS FIELD or AUTO-PROPERTY must wrap the source as a
// vtable-backed enumerator, not store the raw collection pointer. Without the
// wrap, the slot holds a `bz_array_t*` / `bz_llist_t*` / etc. whose first word
// is the backing's count field, NOT a vtable pointer; at disposal the §29.7
// iterator-vtable slot-1 Dispose reads the count bits as a function pointer and
// SIGSEGVs.
//
// Root cause (pre-fix). The field-store dispatch in LlvmBackend_Stmts.cpp
// (`lowerStringFieldStore` / `lowerClosureFieldStore` / `lowerBoxFieldStore` /
// `lowerOwningInlineArrayReassign` / `lowerOwnedCollectionFieldStore` /
// `lowerOwnedClassFieldStore` / `lowerOwnedTaskFieldStore` chain, ~line 941)
// applies each helper when its category matches, and the field-initializer
// codegen path in `emitOwnFieldInit` (LlvmBackend_Setup.cpp ~10650) plain-
// stores the lowered initializer value. Neither site calls
// `wrapAsIEnumerableForTarget` — the helper the local-decl path
// (lowerLocalDecl ~12860) and the RETURN-coercion path
// (wrapAsIEnumerableForTarget ~7555) use. A field/auto-property store of
// `Array<T>` / `Stack<T>` / `Queue<T>` / `LinkedList<T>` / `Set<T>` / a
// declaration-carrying user class implementing `IEnumerable<T>` into an
// `IEnumerable<T>` field therefore leaves the raw backing pointer in the slot,
// and the §29.7 iterator-vtable disposal reads garbage.
//
// THE FIX. Mirror the local-decl / argument / return-coercion wrap at the
// field-store site: before the store, run the source through
// `wrapAsIEnumerableForTarget` so the field holds a proper vtable-backed
// enumerator. The fix is shared between the explicit-assignment path
// (LlvmBackend_Stmts.cpp) and the field-initializer path
// (LlvmBackend_Setup.cpp emitOwnFieldInit) — same predicate
// (declaration-less IEnumerable target), same call.
//
// Auto-property storage is a MemberSymbol, not a FieldSymbol — the existing
// disposal walker in `emitOwnedFieldDisposals` was FieldSymbol-only and so
// silently skipped the backing storage even though the wrap was now in place;
// the wrap's slot-1 Dispose never fired and the wrap frame leaked. The fix
// extends the walker to recognise `owned IEnumerable<T>` auto-property
// storage and classify it into the existing `iterField` (slot-1) arm.
//
// OBSERVABILITY. runAotChecked sets `opts.sanitizeAddress = true` and links the
// program with `-fsanitize=address`. Pre-fix: a SIGSEGV inside the test program
// trips ASan, runAot returns r.ran=false with an "LSan/ASan at exit" or
// "crash/abort" diagnostic, and the wrap frame's bz_array_alloc allocations
// land in the leak report. Post-fix: r.ran=true and r.exitCode=6 (1+2+3) /
// 60 (10+20+30). The for-in positively pins the wrap: a fix that freed the
// backing too early would surface as a wrong sum, not just as a sanitizer
// report.
//
// Test design: each test constructs the source Array via a producer-init
// LOCAL (`var arr := new Array<int>();`) inside the constructor (or as a
// field initializer) and consumes it with `move arr`. The producer-init
// shape makes `returnSourceIsCalleeOwnedCollection` return true (the
// `move e` arm keys on `e`'s initializer, and a NewExpr-init local is
// classified as callee-owned). The wrap-creator therefore emits an OWNING
// enumerator whose slot-1 Dispose (bz_array_free(self->arr, ?) ; free(self))
// reclaims both the wrap frame and the backing array at Holder disposal —
// the source's ownership is fully transferred into the wrap.
//
// Spec citations: §28.6 (interface conformance), §23.1 (auto-property), §23.3
// (computed property), §29.7 (iterator vtable slot-1 Dispose), §28.11
// (owning-collection backing storage).

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"      // runAotChecked — AOT oracle + ASan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// runAotChecked returns r.ran=false (and an Error diagnostic naming the signal)
// when the program crashes or a sanitizer fires — pre-fix every repro below
// either SIGSEGVs at field disposal (the §29.7 broken vtable read) or triggers
// LSan; post-fix every repro exits 0 with no leak. A skip ("SKIP" diagnostic)
// is not expected because the suite requires clang to build, but guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// #2778 canonical repro: an `owned` class FIELD typed `IEnumerable<int>` is
// filled with a moved local `Array<int>`. At class disposal the §29.7 iterator-
// vtable slot-1 Dispose reads the slot's first word as a vtable pointer; pre-
// fix the slot held the raw bz_array_t*'s count field (NOT a vtable pointer)
// → the "vtable" function pointer is `count`'s bits interpreted as code →
// SIGSEGV (the bug's "at disposal"). Post-fix the slot holds a vtable-backed
// OWNING wrapper whose slot-1 Dispose frees the frame AND the backing array,
// exactly once. The for-in in Sum pins the wrapper as the right enumerator
// type, not a raw array. The Holder instance is `using var` so its destructor
// runs before Main returns — exactly the disposal path the bug lives in.
const char* kArrayIntoIEnumerableField =
    "module M; "
    "class Holder "
    "  private owned seq: IEnumerable<int>; "
    "  public constructor() "
    "    var arr := new Array<int>(); "
    "    arr.Add(1); arr.Add(2); arr.Add(3); "
    "    seq := move arr; "
    "  end constructor; "
    "  public method Sum(): int "
    "    var total: int := 0; "
    "    for var x in seq do total := total + x; end for; "
    "    return total; "
    "  end Sum; end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  return h.Sum(); "
    "end Main; end C;";

// Same shape with a class auto-property (the issue's "or auto-property
// equivalent" half). The auto-property's backing storage is field-like, so the
// assignment goes through the same plain-store code path the field test above
// exercises — without the wrap fix this also SIGSEGVs at holder disposal. The
// post-fix disposal walker recognises the autoprop-storage MemberSymbol and
// fires the slot-1 iterator-vtable Dispose, freeing the wrap frame and the
// backing array (the wrap is OWNING because the source is a producer-init
// local). Pre-fix, even with the wrap in place, the autoprop disposal walker
// skipped the backing storage — a SEPARATE pre-existing defect revealed by
// #2778 — leaving the wrap frame leaked; this fix closes both.
const char* kArrayIntoIEnumerableAutoProp =
    "module M; "
    "class Holder "
    "  public owned property Seq: IEnumerable<int>; "
    "  public constructor() "
    "    var arr := new Array<int>(); "
    "    arr.Add(10); arr.Add(20); arr.Add(30); "
    "    Seq := move arr; "
    "  end constructor; "
    "  public method Sum(): int "
    "    var total: int := 0; "
    "    for var x in Seq do total := total + x; end for; "
    "    return total; "
    "  end Sum; end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  return h.Sum(); "
    "end Main; end C;";

// Same shape with a class field INITIALIZER (no constructor body — the store
// happens in emitOwnFieldInit, a separate codegen path from the explicit
// assignment above). The path must wrap too. The initializer is an inline
// NewExpr so the wrap-creator's source-ownership predicate fires
// (`returnSourceIsCalleeOwnedCollection` returns true for `new Array<int>(2)`)
// and the wrap is OWNING — disposal reclaims both the wrap frame and the
// backing storage. The empty array yields a 0 sum, but the load-bearing
// signal is "no SIGSEGV at disposal, no LSan leak" — i.e. the wrap fired and
// its slot-1 Dispose freed the OWNING enumerator's backing.
const char* kArrayIntoIEnumerableFieldInit =
    "module M; "
    "class Holder "
    "  private owned seq: IEnumerable<int> := new Array<int>(2); "
    "  public method Sum(): int "
    "    var total: int := 0; "
    "    for var x in seq do total := total + x; end for; "
    "    return total; "
    "  end Sum; end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  return h.Sum(); "
    "end Main; end C;";

}  // namespace

// ===========================================================================
// #2778 — Array → IEnumerable<T> field store wraps (does not SIGSEGV) §28.6
// ===========================================================================

BEZEL_TEST(codegen_ienumerable_field_init_no_segv_2778,
           array_into_ienumerable_field_wraps_spec_28_6) {
    // §28.6 / §29.7: an `owned` class field receives an `Array<int>` (via
    // `move arr` from a producer-init local) and stores it as a vtable-backed
    // OWNING enumerator. Pre-fix the slot holds the raw bz_array_t*, disposal
    // SIGSEGVs. Post-fix the wrap is correct, the for-in sums 6, and disposal
    // frees the wrap frame + backing array.
    JitResult r = runAotChecked(kArrayIntoIEnumerableField);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

BEZEL_TEST(codegen_ienumerable_field_init_no_segv_2778,
           array_into_ienumerable_autoprop_wraps_spec_28_6) {
    // The auto-property form. The backing storage is field-like, so the same
    // field-store code path runs — and the same wrap fix applies. The
    // post-fix disposal walker also visits the autoprop-storage MemberSymbol
    // so the slot-1 Dispose reclaims the OWNING wrap.
    JitResult r = runAotChecked(kArrayIntoIEnumerableAutoProp);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

BEZEL_TEST(codegen_ienumerable_field_init_no_segv_2778,
           field_init_array_into_ienumerable_wraps_spec_28_6) {
    // emitOwnFieldInit's plain-store path: a field with no constructor body
    // but an initializer expression. The wrapping must happen at the
    // initializer site, not at a later assignment.
    JitResult r = runAotChecked(kArrayIntoIEnumerableFieldInit);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}