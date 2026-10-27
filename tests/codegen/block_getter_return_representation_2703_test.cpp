// Codegen — a BLOCK-bodied property/indexer getter never applied any of the
// four REPRESENTATION conversions its `return` needs, because all four are
// gated on `currentReturnTypeNode_` and that field is deliberately null inside
// an accessor body. Issue #2703.
//
// Spec (revision v0.240):
//   §23.3 "Block-form properties" — "Each accessor uses `=> expression` or
//     `=> do ... end`." A `do … end` accessor is a user statement body whose
//     `return` terminates the ACCESSOR, so it carries the ACCESSOR's declared
//     type as its return contract.
//   §23.4 "Indexers" — the same accessor forms for the indexer spelling.
//   §8.6 "Boxing" — a value returned where `Object`/an interface is expected
//     must be boxed, or the caller's `is`/`as`/virtual dispatch has no box to
//     walk.
//   §29.2 "Desugaring of `for`" — a value handed out as `IEnumerable<T>` must
//     be a vtable-backed enumerator; the caller's for-in dispatches through it.
//   §15.1 "Inline arrays" — `[N]T` is a value type returned BY VALUE, built
//     from a `return [e1, …]` against the declared element/extent.
//   Axiom A4 (spec line 6351) — "Accessor read faces are lend-only: property
//     and indexer *types* carry no `owned`, and a getter may not return a fresh
//     owned value". This is why the fix must NOT re-enable the +1 mint arms.
//
// THE BUG. `currentReturnTypeNode_` is one field encoding two independent
// things: how a returned value must be REPRESENTED, and whether the callee owes
// the caller a +1 for a `string`/closure result. #2694 nulled it inside the
// block-getter emitters to stop the second (A4: an accessor read face is +0,
// and no call site discharges a mint) — which correctly killed the first as
// well, since they share the field. This fix carries the representation half in
// its own pair of members (`accessorReturnTypeNode_` / `accessorReturnTypeFn_`,
// handed out by `returnReprTypeNode()` only inside the accessor function that
// registered them), so the two mint arms keep keying on `currentReturnTypeNode_`
// and stay off.
//
// WHAT HAPPENED PRE-FIX, PER ARM (each reproduced mechanically on the parent
// commit, exit codes as observed):
//   §8.6 boxing              hard build failure — "Function return type does
//                            not match operand type of return inst! ret i32 %2
//                            / ptr", IR verification rejected the module.
//   §29.2 IEnumerable wrap   SIGSEGV (exit 139) — the caller's for-in drove a
//                            raw `bz_array_t*` through the §29.11 enumerator
//                            vtable. The most dangerous of the four.
//   nullable coercion        build failure for the shapes with no backstop
//                            ("ret i32 42" against `{ i8, i64 }`). NOTE: a
//                            plain `int?` getter survives without the fix —
//                            emitReturn has its own EXACT-WIDTH nullable-prim
//                            fallback — so this file pins the WIDENING shape
//                            (`long?` returning an `int` literal), which only
//                            `coerceReturnToNullableValue` handles (#843).
//   collection literal       SILENT ZERO (exit 0 where 66 was expected), with
//                            only the #2491 warning. No crash, no failed build:
//                            the worst failure mode of the four.
//
// ⚠ NOT FIXED HERE, AND DELIBERATELY NOT TESTED AS FIXED: §8.6 boxing on the
// VTABLE/ITABLE face (`ensurePropertyAccessorFn`). That emitter runs inside
// `declareVtables()`, which precedes `setupBoxDescriptors()`, so `boxVtableFor`
// finds no descriptor and `boxValue` is a no-op there — a PASS-ORDERING defect
// of the #2500 family, unchanged by this commit (it fails identically before
// and after). Filed separately. The other three arms DO work on that face and
// are covered below.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// ---------------------------------------------------------------------------
// (A) §8.6 boxing — an `Object`-typed block getter returning an `int`.
// Pre-fix: IR verification failure (the module does not build at all).
// ---------------------------------------------------------------------------
const char* kObjectBlockGetter =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property O: Object "
    "    get => do "
    "      return _n; "
    "    end; "
    "  end O; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var o := h.O; "
    "    if o is int then return 41; end; "   // 41 iff a real box was returned
    "    return 1; "
    "  end Main; "
    "end P;";

// (A2) The INDEXER spelling of the same arm (§23.4). The body returns
// `_n + i`, so the index parameter is live too — a getter that ignored its
// argument would still return 7+0 and pass, hence the `+ i`.
const char* kObjectBlockIndexer =
    "module M; "
    "class Bag "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public property [i: int]: Object "
    "    get => do "
    "      return _n + i; "
    "    end; "
    "  end; "
    "end Bag; "
    "class P "
    "  public static method Main(): int "
    "    var b := new Bag(7); "
    "    var o := b[1]; "
    "    if o is int then return 27; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// ---------------------------------------------------------------------------
// (B) §29.2 IEnumerable wrapping — an `IEnumerable<int>` block getter whose
// body returns an `Array<int>` field. Pre-fix: SIGSEGV in the caller's for-in.
// ---------------------------------------------------------------------------
const char* kIEnumerableBlockGetter =
    "module M; "
    "class Holder "
    "  private _a: Array<int> := [3, 4, 5]; "
    "  public constructor() end constructor; "
    "  public property E: IEnumerable<int> "
    "    get => do "
    "      return _a; "
    "    end; "
    "  end E; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var total := 0; "
    "    for var x in h.E do total := total + x; end; "
    "    return total; "                       // 3+4+5 = 12
    "  end Main; "
    "end P;";

// (B2) THE DOUBLE-FREE AXIS. The §29.2 wrap ALLOCATES a wrapper frame, so the
// sharp question this fix has to answer is who disposes it — and whether the
// wrap could ever be the OWNING variant over a field the object also disposes
// (that would be a double free, not a leak).
//
// It cannot: `returnSourceIsCalleeOwnedCollection` classifies a FIELD READ as a
// borrow (only `new T(…)`, `move e`, and a producer-initialised LOCAL are
// callee-owned), so the borrowing wrap is used and its slot-1 Dispose frees the
// wrapper frame ONLY. The frame in turn is disposed by the reader: a for-in
// whose source is a NON-auto property is classified `!sourceIsNamedLocal`
// (lowerForIn, #1840/#2485) and registers the loop-end `iterEnumDispose`.
//
// This case makes that argument mechanical rather than asserted: an `owned
// OwningArray<Item>` field, whose elements the owning collection disposes and
// whose backing the object's owned-field cascade disposes, enumerated 16 times
// through the block getter under ASan+LSan. A leaked wrapper frame is LSan-red;
// an owning wrap over the field is a double free ASan reports; 16 iterations
// amplify either past a single-allocation coincidence.
const char* kOwningFieldIEnumerable =
    "module M; "
    "class Item "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item; "
    "class Holder "
    "  private owned _a: OwningArray<Item>; "
    "  public constructor() "
    "    _a := new OwningArray<Item>(); "
    "    _a.Add(new Item(3)); "
    "    _a.Add(new Item(4)); "
    "  end constructor; "
    "  public property E: IEnumerable<Item> "
    "    get => do "
    "      return _a; "
    "    end; "
    "  end E; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..16 do "
    "      using var h := new Holder(); "
    "      for var x in h.E do total := total + x.V; end; "
    "    end; "
    "    if total = 112 then return 25; end; "   // 16 * (3+4)
    "    return 1; "
    "  end Main; "
    "end P;";

// ---------------------------------------------------------------------------
// (C) `coerceReturnToNullableValue` — the arm #2716 reports for the LAMBDA body
// form. `long?` returning the `int` literal 42 needs the #843 narrower-numeric
// widen that ONLY this call site performs; emitReturn's own nullable-prim
// fallback requires an exact width match and therefore does not fire.
// ---------------------------------------------------------------------------
const char* kNullableWideningBlockGetter =
    "module M; "
    "class Holder "
    "  public constructor() end constructor; "
    "  public property L: long? "
    "    get => do "
    "      return 42; "
    "    end; "
    "  end L; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var q := h.L; "
    "    if q = null then return 9; end; "
    "    return 60; "
    "  end Main; "
    "end P;";

// (C2) CONTROL — the plain `int?` shape, which ALREADY worked pre-fix via
// emitReturn's exact-width fallback. It must keep working: this pins that the
// new coercion does not double-wrap an already-tagged struct.
const char* kNullableExactBlockGetter =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor() _n := 5; end constructor; "
    "  public property Q: int? "
    "    get => do "
    "      return _n; "
    "    end; "
    "  end Q; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var q := h.Q; "
    "    if q = null then return 9; end; "
    "    return q! + 30; "                      // 35
    "  end Main; "
    "end P;";

// ---------------------------------------------------------------------------
// (D) Collection-literal return materialisation — the SILENT one. An inline
// array (`[3]int`, a §15.1 VALUE type: no allocation, no ownership) built from
// a literal in the getter body. Pre-fix `retTargetTy` stayed null, `v` stayed
// null, and the getter returned a zero aggregate with only a warning.
// ---------------------------------------------------------------------------
const char* kInlineArrayBlockGetter =
    "module M; "
    "class Holder "
    "  public constructor() end constructor; "
    "  public property A: [3]int "
    "    get => do "
    "      return [11, 22, 33]; "
    "    end; "
    "  end A; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var a := h.A; "
    "    return a[0] + a[1] + a[2]; "           // 66 — pre-fix this was 0
    "  end Main; "
    "end P;";

// ---------------------------------------------------------------------------
// (E) The VTABLE/ITABLE face (`ensurePropertyAccessorFn`): the same block
// getter body reached through an INTERFACE reference. Covers the §29.2 and
// nullable arms on that face; §8.6 boxing there is blocked by the separate
// pass-ordering defect noted in the header and is NOT asserted.
// ---------------------------------------------------------------------------
const char* kIfaceIEnumerable =
    "module M; "
    "interface ISeq  readonly property E: IEnumerable<int>; end ISeq; "
    "class Holder : ISeq "
    "  private _a: Array<int> := [3, 4, 5]; "
    "  public constructor() end constructor; "
    "  public property E: IEnumerable<int> "
    "    get => do "
    "      return _a; "
    "    end; "
    "  end E; "
    "end Holder; "
    "class P "
    "  public static method SumOf(s: ISeq): int "
    "    var total := 0; "
    "    for var x in s.E do total := total + x; end; "
    "    return total; "
    "  end SumOf; "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var f: ISeq := h; "
    "    return P.SumOf(f); "                   // 12
    "  end Main; "
    "end P;";

const char* kIfaceNullableWidening =
    "module M; "
    "interface INum  readonly property L: long?; end INum; "
    "class Holder : INum "
    "  public constructor() end constructor; "
    "  public property L: long? "
    "    get => do "
    "      return 42; "
    "    end; "
    "  end L; "
    "end Holder; "
    "class P "
    "  public static method Read(n: INum): int "
    "    var q := n.L; "
    "    if q = null then return 9; end; "
    "    return 61; "
    "  end Read; "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var f: INum := h; "
    "    return P.Read(f); "
    "  end Main; "
    "end P;";

// ---------------------------------------------------------------------------
// (F) OVER-CORRECTION GUARDS — the whole risk of this fix. Setting a return
// node re-enables EVERY arm keyed on it, and two of those arms MINT a +1.
// Axiom A4 forbids that for an accessor read face, and #2694 measured the
// consequence (LSan: 20 strings over 20 iterations). These two cases go RED if
// the mint ever fires from an accessor.
//
// (F1) `string`: the exact #2694 subject. The getter returns a BORROW of a
// field, so a mint here is one unreleased +1 per read. Strings are built at
// RUN TIME with `$"v-{i}"` — `"ab" + "cd"` constant-folds to an RC sentinel
// that `bz_string_release` no-ops on, which makes a literal-built probe
// vacuous. 20 iterations, both a `string`-contract reader (the one whose
// contract used to be inherited) and an `int`-contract reader.
// ---------------------------------------------------------------------------
const char* kStringBlockGetterNoMint =
    "module M; "
    "class Holder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public property S: string "
    "    get => do "
    "      return _s; "
    "    end; "
    "  end S; "
    "end Holder; "
    "class P "
    "  public static method LenOf(h: Holder): int "
    "    return h.S.Length; "
    "  end LenOf; "
    "  public static method StrOf(h: Holder): string "
    "    return h.S; "
    "  end StrOf; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var h := new Holder($\"v-{i}\"); "
    "      total := total + P.LenOf(h); "
    "      var t := P.StrOf(h); "
    "      total := total + t.Length; "
    "    end; "
    "    if total = 142 then return 26; end; "   // 2 * 71
    "    return 1; "
    "  end Main; "
    "end P;";

// (F2) CLOSURE: the #2691 category's accessor twin. A `(int) -> int`-typed
// block getter returning a closure FIELD is a borrow; a mint here would be an
// unreleased +1 on the closure cell per read (32 iterations amplify it).
const char* kClosureBlockGetterNoMint =
    "module M; "
    "class Holder "
    "  private _f: (int) -> int; "
    "  public constructor() "
    "    _f := lambda(x: int): int => x + 1; "
    "  end constructor; "
    "  public property F: (int) -> int "
    "    get => do "
    "      return _f; "
    "    end; "
    "  end F; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..32 do "
    "      using var h := new Holder(); "
    "      var g := h.F; "
    "      total := total + g(i); "
    "    end; "
    "    if total = 560 then return 29; end; "   // sum(2..33) = 560
    "    return 1; "
    "  end Main; "
    "end P;";

}  // namespace

// ===========================================================================
// (A) §8.6 boxing. // per §8.6, §23.3 (issue #2703)
// ===========================================================================
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           object_block_getter_boxes_its_returned_value_spec_8_6) {
    JitResult r = runAot(kObjectBlockGetter);
    BEZEL_EXPECT_TRUE(r.ran);       // pre-fix: IR verification rejected the module
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §8.6, §23.4
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           object_block_indexer_boxes_its_returned_value_spec_23_4) {
    JitResult r = runAot(kObjectBlockIndexer);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 27);
}

// ===========================================================================
// (B) §29.2 IEnumerable wrapping. // per §29.2, §23.3
// ===========================================================================
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           ienumerable_block_getter_wraps_array_field_spec_29_2) {
    JitResult r = runAot(kIEnumerableBlockGetter);
    BEZEL_EXPECT_TRUE(r.ran);       // pre-fix: SIGSEGV in the caller's for-in
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// ⚠ NO `runAotChecked` COMPANION FOR kIEnumerableBlockGetter, deliberately.
// That program leaks its `Array<int>` FIELD INITIALISER (`private _a:
// Array<int> := [3,4,5];` — a borrowing view type with no owner), a
// PRE-EXISTING defect unrelated to this fix: the byte-identical leak (128
// allocations / 7168 bytes, every one from `bz_array_alloc` inside
// `Holder$ctor`) reproduces with the property replaced by an ordinary METHOD,
// which never takes this code path. LeakSanitizer forces the process exit code
// to 1 on any leak, so such a case could assert neither `ran` nor the value.
// The OWNING-field case immediately below is the leak-clean ASan oracle for
// this same arm, and it is the stronger one anyway — it is the shape where a
// wrong wrap variant would be a DOUBLE FREE rather than a leak.

// THE DOUBLE-FREE / LEAK AXIS — see kOwningFieldIEnumerable's note.
// per §29.2, §28.5, §28.9
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           ienumerable_block_getter_owning_field_exactly_once_spec_29_2) {
    JitResult r = runAotChecked(kOwningFieldIEnumerable);
    BEZEL_EXPECT_TRUE(r.ran);        // ASan+LSan clean over 16 enumerations
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

// The same program on the PLAIN oracle (#2633/#2629).
// per §29.2, §28.5
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           ienumerable_block_getter_owning_field_plain_spec_29_2) {
    JitResult r = runAot(kOwningFieldIEnumerable);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

// ===========================================================================
// (C) Nullable-value return coercion. // per §7.2, §5.1 (the widen), §23.3
// ===========================================================================
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           nullable_widening_block_getter_coerces_spec_7) {
    JitResult r = runAot(kNullableWideningBlockGetter);
    BEZEL_EXPECT_TRUE(r.ran);       // pre-fix: "ret i32 42" vs `{ i8, i64 }`
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// CONTROL — already worked pre-fix via emitReturn's exact-width fallback, and
// must not be double-wrapped by the new coercion. // per §7, §23.3
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           nullable_exact_width_block_getter_control_spec_7) {
    JitResult r = runAot(kNullableExactBlockGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 35);
}

// ===========================================================================
// (D) Collection-literal materialisation — the silent-wrong-value arm.
// // per §15.1, §23.3
// ===========================================================================
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           inline_array_block_getter_materializes_literal_spec_15_1) {
    JitResult r = runAot(kInlineArrayBlockGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 66);  // pre-fix: 0, silently
}

// ===========================================================================
// (E) The vtable/itable face. // per §23.3, §29.2, §13
// ===========================================================================
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           ienumerable_block_getter_via_interface_spec_29_2) {
    JitResult r = runAot(kIfaceIEnumerable);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// per §7, §23.3, §13
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           nullable_widening_block_getter_via_interface_spec_7) {
    JitResult r = runAot(kIfaceNullableWidening);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 61);
}

// ===========================================================================
// (F) OVER-CORRECTION GUARDS — a mint that fires from an accessor read face is
// exactly what axiom A4 forbids and what #2694 measured. Both cases pass
// BEFORE and AFTER this fix by design: their job is to fail if a future change
// (or this one, done wrong) re-enables the mint arms.
// // per A4 (spec line 6351), §28.9, §23.3
// ===========================================================================
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           string_block_getter_still_mints_nothing_axiom_a4) {
    JitResult r = runAotChecked(kStringBlockGetterNoMint);
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean: no unreleased +1 per read
    BEZEL_EXPECT_EQ(r.exitCode, 26);
}

// per A4 (spec line 6351), §27.5, §28.9
BEZEL_TEST(codegen_block_getter_return_repr_2703,
           closure_block_getter_still_mints_nothing_axiom_a4) {
    JitResult r = runAotChecked(kClosureBlockGetterNoMint);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 29);
}
