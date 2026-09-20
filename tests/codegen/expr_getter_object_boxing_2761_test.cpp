// Codegen — an EXPRESSION-bodied property/indexer getter whose declared type is
// `Object` (or an interface) never boxed its value-typed body at all, so
// `o is int` was silently FALSE and an `Object`-typed argument failed IR
// verification. Issue #2761.
//
// Spec (revision v0.240):
//   §8.6 "Boxing" — "When a value type is used where `Object` (or an interface
//     implemented by the value type) is expected, the value is **boxed**: a
//     heap-allocated wrapper holds the value. Boxed values are runtime-managed
//     (§28.9)."
//   §23.2 "Expression-bodied read-only properties" — "An expression-bodied
//     property's (or indexer's) body is checked against the declared type".
//     The declared type is therefore the representation contract of the read,
//     and §8.6 is what that contract requires when the type is `Object`.
//   §23.3 "Block-form properties" — the sibling body form, whose §8.6 arm
//     #2703 fixed. §23.4 "Indexers" — the indexer spelling of both.
//   §12.7 "Type tests: `is`, `is not`, `as`" / §17.4 "The `is`, `is not`, and
//     `as` operators" — what the caller does with the box.
//   §28.9 "Runtime-managed types" — "Boxed value types — created implicitly
//     when a value type is used where `Object` (or an interface) is expected"
//     are "exempt from manual disposal". This is why the fix adds no free.
//
// THE BUG. An expression-bodied getter is INLINED at the read site (or, on the
// thunk/vtable faces, `CreateRet`'d directly); it never reaches `lowerReturn`,
// so none of the representation arms #2703 restored apply to it. The inline
// applied only `wrapNullableGetterResult` and handed the caller the body's
// natural value. Every DOWNSTREAM `boxForTarget` keys on the sema type of its
// SOURCE EXPRESSION — which for a property read is already `Object` — so all of
// them no-op, and a raw `i32` travelled into `Object` positions unchecked.
//
// SYMPTOMS ON 0e554e3, each reproduced (this file pins every one):
//   `var o := h.O; o is int`   SILENT FALSE — exit 1 where 41 was expected, no
//                              diagnostic. `lowerTypeTest` (LlvmBackend_Layout
//                              .cpp) returns null for a non-pointer operand
//                              whose sema type is `Object`, and `lowerIf`'s
//                              null-condition arm branches straight past the
//                              body: the emitted IR literally reads
//                              "if.body: No predecessors!".
//   `o as int`                 SILENT null (the same bail, `as` arm).
//   `o.ToString()`             wrong string.
//   `F(h.O)` for `F(o: Object)` HARD BUILD FAILURE — "Call parameter type does
//                              not match function signature!".
//   `() -> Object := h.O`      HARD BUILD FAILURE — the §27.6 property thunk
//                              `ret i32` from a `ptr`-returning function.
//   `Current: Object => 7` on  SIGSEGV, exit 139 — the WORST of the set. The
//   a duck-typed enumerator     for-in bound the raw scalar as the loop
//                               variable and the body drove it as a pointer.
//   the bare, static, accessor-`get =>`, indexer, interface-typed, `bool` and
//   `enum` spellings — all silently false.
//
// This inverts the #2688 body-form agreement invariant: since #2703 the BLOCK
// form of the identical property returned 41 while the EXPRESSION form returned
// 1. Both forms are now pinned here, and to the SAME value, so a future
// divergence in EITHER direction goes red.
//
// ⚠ THE LEAK THIS BLOCK USED TO RECORD IS FIXED (#2760, v0.306). It read:
// "THE BOX LEAKS, KNOWINGLY, EXACTLY AS THE BLOCK FORM'S DOES … who owes the
// release is #2760's open question for BOTH forms." §28.9's *Accessor faces
// are uniform across type kinds* paragraph answered that question — a getter's
// fresh runtime-managed production "remains admissible … and the
// implementation reclaims the production; how is not specified" — and #2760
// implemented it for both §23.3 body spellings at once, keying the release on
// the READER'S BINDING. The leak-clean rows for these very programs now live
// in object_property_getter_box_reclaim_2760_test.cpp (×200, runAotChecked).
//
// The boxing cases below still use the PLAIN oracle, which is now a
// CONSERVATIVE choice rather than a necessary one: these rows exist to pin the
// #2761 VALUE (the expression form must agree with the block form), and the
// leak oracle for the same shapes is #2760's file. The two over-correction
// guards, where nothing should be minted at all, DO run checked.
//
// ⚠ NOT FIXED, AND DELIBERATELY PINNED AS STILL-BROKEN BELOW: the
// VIRTUAL/vtable face (`public virtual property O: Object => 0;`). That
// emitter runs inside `declareVtables()`, which precedes
// `setupBoxDescriptors()`, so `boxVtableFor` returns null and `boxValue` is
// inert. MEASURED by instrumenting `boxValue`: on the virtual repro it is
// reached twice (`bz$Base$O$propget`, `bz$Derived$O$propget`) with
// `primBoxVtable_.size == 0`, while the inlined read sees size 18. That is
// #2759 — the identical gap #2703 hit on the block face — and it is not
// fixable by swapping the two passes, because `setupBoxDescriptors` is
// documented at its call site as requiring the descArray_/vtable globals
// `declareVtables` builds. The residual is a BUILD FAILURE, never a silent
// wrong value, so it is the safe direction to leave standing.

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

// The issue's own repro, expression body. 41 iff a real box reached the caller.
const char* kObjectExprGetter =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property O: Object => _n; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var o := h.O; "
    "    if o is int then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// The BLOCK-bodied twin of the byte-identical program. #2688's precedent is
// that asserting AGREEMENT is weaker than asserting a pinned value, so both
// forms are pinned to 41 rather than compared to each other.
const char* kObjectBlockGetterTwin =
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
    "    if o is int then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// §17.4: `o as int` yields `int?`. This pins the PAYLOAD, not just the tag —
// a box whose descriptor matched but whose bytes were wrong would return 34.
const char* kObjectExprGetterUnbox =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property O: Object => _n; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var o := h.O; "
    "    var r: int? := o as int; "
    "    return (r ?? 0) + 34; "   // 7 + 34 = 41
    "  end Main; "
    "end P;";

// ARGUMENT position. Pre-fix this did not build at all: "Call parameter type
// does not match function signature!" — the loudest of the symptoms, and the
// one that proves the defect is a REPRESENTATION defect rather than a
// type-test defect.
const char* kObjectExprGetterArgument =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property O: Object => _n; "
    "end Holder; "
    "class P "
    "  private static method F(o: Object): int "
    "    if o is int then return 41; end; "
    "    return 1; "
    "  end F; "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    return P.F(h.O); "
    "  end Main; "
    "end P;";

// §23.3 accessor-level expression body (`get => expr;`) — the THIRD spelling.
// Distinct AST shape (PropertyBodyKind::Block with an Expression accessor),
// same inline lowering, same defect.
const char* kObjectAccessorExprGetter =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property O: Object get => _n; end O; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var o := h.O; "
    "    if o is int then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// §23.4 indexer spelling. The body reads `_n + i`, so an accessor that ignored
// its index would still yield 7 and could not pass with i = 1.
const char* kObjectExprIndexer =
    "module M; "
    "class Bag "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public property [i: int]: Object => _n + i; "
    "end Bag; "
    "class P "
    "  public static method Main(): int "
    "    var b := new Bag(7); "
    "    var o := b[1]; "
    "    var r: int? := o as int; "
    "    return (r ?? 0) + 33; "   // 8 + 33 = 41
    "  end Main; "
    "end P;";

// A STATIC expression-bodied property: its own early-return arm in the
// qualified-read lowering, which never reaches the instance path's box.
const char* kObjectExprStaticProperty =
    "module M; "
    "class Holder "
    "  public static property O: Object => 7; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var o := Holder.O; "
    "    if o is int then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// The BARE (implicit-`this`) read inside the declaring class — a separate
// lowering arm from the qualified `h.O` read.
const char* kObjectExprBareThisRead =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property O: Object => _n; "
    "  public method Probe(): int "
    "    var o := O; "
    "    if o is int then return 41; end; "
    "    return 1; "
    "  end Probe; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    return h.Probe(); "
    "  end Main; "
    "end P;";

// §8.6's INTERFACE half ("or an interface implemented by the value type"):
// a record-typed body handed out through an interface-typed property.
const char* kInterfaceExprGetterRecord =
    "module M; "
    "interface IShape method Area(): int; end IShape; "
    "record Sq(s: int) : IShape "
    "  public method Area(): int => s * s; "
    "end Sq; "
    "class Holder "
    "  private _r: Sq; "
    "  public constructor() _r := new Sq(3); end constructor; "
    "  public property S: IShape => _r; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var o := h.S; "
    "    if o is Sq then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// A non-`int` boxable value kind, so the fix is not pinned to one primitive.
const char* kObjectExprGetterEnum =
    "module M; "
    "enum Color Red, Green; end Color; "
    "class Holder "
    "  public property O: Object => Color.Green; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var o := h.O; "
    "    if o is Color then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// §27.6 property-as-function-value. `makePropertyThunk` builds a thunk typed
// from `pd->type`, so a value body under an `Object` declared type emitted
// `ret i32` from a `ptr`-returning thunk. Pre-fix: hard build failure.
const char* kObjectExprGetterAsFunctionValue =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property O: Object => _n; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var f: () -> Object := h.O; "
    "    var o := f(); "
    "    var r: int? := o as int; "
    "    return (r ?? 0) + 34; "   // 7 + 34 = 41
    "  end Main; "
    "end P;";

// §29.11 duck-typed enumerator whose `Current` is an expression-bodied
// `Object` property. THE WORST FAILURE MODE OF THIS DEFECT: pre-fix the loop
// variable took the raw scalar and the body drove it as a pointer — SIGSEGV,
// exit 139 (reproduced on 0e554e3). The block-bodied twin of this shape has
// been safe since #2703, which called a compiled getter.
const char* kObjectExprCurrentInUserEnumerator =
    "module M; "
    "class E "
    "  private _i: int; "
    "  public constructor() _i := 0; end constructor; "
    "  public method MoveNext(): bool "
    "    _i := _i + 1; "
    "    return _i <= 1; "
    "  end MoveNext; "
    "  public property Current: Object => 7; "
    "end E; "
    "class Src "
    "  public method GetEnumerator(): E => new E(); "
    "end Src; "
    "class P "
    "  public static method Main(): int "
    "    var s := new Src(); "
    "    var t := 0; "
    "    for var v in s do "
    "      if v is int then t := t + 41; end; "
    "    end; "
    "    return t; "
    "  end Main; "
    "end P;";

// ---------------------------------------------------------------------------
// OVER-CORRECTION GUARDS. §8.6 boxes a VALUE type only; a reference source
// reaches `Object` as itself. If the new call boxed a reference, `o is Inner`
// would walk a box descriptor instead of the class's own and go false — and,
// under LSan, 64 reads would leak 64 wrappers that nothing releases. Both
// directions are asserted: the VALUE (positively) and the absence of a mint.
// ---------------------------------------------------------------------------
const char* kClassExprGetterNoBox =
    "module M; "
    "class Inner public v: int; end Inner; "
    "class Holder "
    "  private owned _i: Inner; "
    "  public constructor() _i := new Inner(); end constructor; "
    "  public property O: Object => _i; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var hits := 0; "
    "    for var i in 1..64 do "
    "      using var h := new Holder(); "
    "      var o := h.O; "
    "      if o is Inner then hits := hits + 1; end; "
    "    end; "
    "    if hits = 64 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

const char* kStringExprGetterNoBox =
    "module M; "
    "class Holder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public property O: Object => _s; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var hits := 0; "
    "    for var i in 1..64 do "
    "      using var h := new Holder($\"v-{i}\"); "
    "      var o := h.O; "
    "      if o is string then hits := hits + 1; end; "
    "    end; "
    "    if hits = 64 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// A non-Object declared type must be untouched: `boxForTarget` no-ops unless
// the TARGET is Object/an interface. Guards the whole ordinary-property surface
// the new call sits on.
const char* kIntExprGetterUnchanged =
    "module M; "
    "class Holder "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property N: int => _n * 3; "
    "  public property M: int? => _n; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    return h.N + (h.M ?? 0) + 13; "   // 21 + 7 + 13 = 41
    "  end Main; "
    "end P;";

// The residual: #2759. Pinned as STILL FAILING TO BUILD so that whoever fixes
// the pass ordering sees this test go red and knows to flip it to `41`.
const char* kVirtualExprGetterStillBroken =
    "module M; "
    "class Base "
    "  public virtual property O: Object => 0; "
    "end Base; "
    "class Derived : Base "
    "  public override property O: Object => 7; "
    "end Derived; "
    "class P "
    "  public static method Main(): int "
    "    var d := new Derived(); "
    "    var b: Base := d; "
    "    var o := b.O; "
    "    if o is int then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

}  // namespace

// ===========================================================================
// (A) §8.6 boxing at the inlined expression-getter read. // per §8.6, §23.2
// ===========================================================================
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_expr_getter_boxes_its_value_spec_8_6) {
    JitResult r = runAot(kObjectExprGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // pre-fix: 1, silently, no diagnostic
}

// per §8.6, §23.3 — the body-form AGREEMENT half of #2688's invariant, pinned
// by VALUE on both sides rather than by equality.
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_block_getter_twin_agrees_by_value_spec_23_3) {
    JitResult blk = runAot(kObjectBlockGetterTwin);
    BEZEL_EXPECT_TRUE(blk.ran);
    BEZEL_EXPECT_EQ(blk.exitCode, 41);   // green since #2703
    JitResult exp = runAot(kObjectExprGetter);
    BEZEL_EXPECT_TRUE(exp.ran);
    BEZEL_EXPECT_EQ(exp.exitCode, 41);   // pre-fix: 1 — the disagreement
}

// per §17.4 — the unboxed PAYLOAD, not just the type tag.
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_expr_getter_unboxes_to_its_payload_spec_17_4) {
    JitResult r = runAot(kObjectExprGetterUnbox);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // pre-fix: 34 (`as` yielded null)
}

// per §8.6 — argument position. Pre-fix this program did not BUILD.
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_expr_getter_argument_position_spec_8_6) {
    JitResult r = runAot(kObjectExprGetterArgument);
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: false — IR verification rejected it
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (B) Every other spelling of the same read. // per §23.2, §23.3, §23.4
// ===========================================================================
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_accessor_expr_getter_boxes_spec_23_3) {
    JitResult r = runAot(kObjectAccessorExprGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §23.4
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_expr_indexer_boxes_spec_23_4) {
    JitResult r = runAot(kObjectExprIndexer);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // pre-fix: 33 (`as` yielded null)
}

// per §8.6, §23.2
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_expr_static_property_boxes_spec_8_6) {
    JitResult r = runAot(kObjectExprStaticProperty);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §8.6, §23.2
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_expr_bare_this_read_boxes_spec_8_6) {
    JitResult r = runAot(kObjectExprBareThisRead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §8.6 — the interface half of the boxing rule.
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           interface_expr_getter_boxes_record_spec_8_6) {
    JitResult r = runAot(kInterfaceExprGetterRecord);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §8.6, §10 — a second boxable value kind.
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_expr_getter_boxes_enum_spec_8_6) {
    JitResult r = runAot(kObjectExprGetterEnum);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §8.6, §27.6 — pre-fix this program did not BUILD.
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_expr_getter_as_function_value_boxes_spec_27_6) {
    JitResult r = runAot(kObjectExprGetterAsFunctionValue);
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: false — IR verification rejected it
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §8.6, §29.11 — pre-fix this program SIGSEGV'd (exit 139). The exit-code
// assertion alone is the oracle: 139 masked as anything else would still not
// be 41, and `ran` is true for a signalled run.
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           object_expr_current_in_user_enumerator_boxes_spec_29_11) {
    JitResult r = runAot(kObjectExprCurrentInUserEnumerator);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // pre-fix: 139 (SIGSEGV)
}

// ===========================================================================
// (C) Over-correction guards — green BEFORE and AFTER the fix.
// ===========================================================================
// per §8.6 ("a VALUE type used where Object is expected"): a class reference
// is not boxed. Asserted on BOTH oracles: the value pins the descriptor walk,
// and the checked run pins that nothing was minted (64 reads, one wrapper each
// if the guard were wrong).
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           class_expr_getter_mints_no_box_spec_8_6) {
    JitResult plain = runAot(kClassExprGetterNoBox);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kClassExprGetterNoBox);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // LSan forces 1 on any leak
}

// per §8.6, §28.9 (`string` is runtime-managed, never boxed).
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           string_expr_getter_mints_no_box_spec_28_9) {
    JitResult plain = runAot(kStringExprGetterNoBox);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kStringExprGetterNoBox);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §23.2, §7 — a non-Object declared type is untouched by the new call.
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           non_object_expr_getter_unchanged_spec_23_2) {
    JitResult plain = runAot(kIntExprGetterUnchanged);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kIntExprGetterUnchanged);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (D) The RESIDUAL, pinned as still-broken. // per §8.6, §12 (issue #2759)
// ===========================================================================
// The virtual/vtable face still fails IR verification, because `boxValue` is
// inert inside `declareVtables()` (measured: primBoxVtable_ is empty there).
// This asserts the residual is a BUILD FAILURE — never a silent wrong value —
// so a regression into silence goes red, and so the #2759 fix flips this test
// rather than leaving the shape unwatched.
BEZEL_TEST(codegen_expr_getter_object_boxing_2761,
           virtual_expr_getter_still_unbuildable_issue_2759) {
    JitResult r = runAot(kVirtualExprGetterStillBroken);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
