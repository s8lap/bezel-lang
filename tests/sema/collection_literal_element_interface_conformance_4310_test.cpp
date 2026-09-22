// fix #4310 / spec §5.1 *Implicit conversions* / §11.2 *Interface
// declaration* (v0.298). §11.2 states conformance as: "A class implements
// an interface implicitly: by declaring matching public members" — read
// together with §11.1's "A class may declare ... any number of interfaces"
// (the `: I1, I2` inheritance-clause list a conforming class must appear
// in): the "implicitly" clause waives per-member `override` annotations, it
// does not waive LISTING the interface. This is the reading
// `checkAssignable`'s existing `isSubtype` walk already enforces — verified
// correct here at the two "_control" rows below before this fix touched
// anything.
//
// ROOT CAUSE (traced, ExprTypecheck.cpp / ExprTypecheck_Calls.cpp).
// `ExprTC::checkAssignable` performs the §5.1 conversion check — including
// the class->interface §11.2 nominal-conformance test via `isSubtype` — and
// it correctly fires for a plain variable declaration and an ordinary call
// argument (see `plain_local_declaration_noconform_rejected_control` /
// `plain_call_argument_noconform_rejected_control` below). But every site
// that types a COLLECTION LITERAL against a known target type pre-sets the
// literal's own `exprType` to the target type (`ctx_.setExprType`) before
// running the general `checkAssignable(ctx_.exprType(*literal), target, …)`
// call that follows — so that call compares the target type to itself and
// trivially passes, never descending into the literal's own element
// expressions. A class that never lists the target interface in its
// inheritance clause is therefore silently accepted into an interface-typed
// collection element, and interface dispatch on it reads a null itable slot
// — the SIGSEGV #4302 measured.
//
// THE FIX is `ExprTC::checkCollectionLiteralElementAssignability`
// (ExprTypecheck.cpp), which re-runs the ordinary `checkAssignable` judgment
// PER ELEMENT (and per dict key/value) of a collection literal, called from
// every site that already types a literal against a target: LocalDecl,
// LocalConstDecl (a DIFFERENT AST node from LocalDecl — §21.1's runtime
// `const`), FieldDecl / PropertyDecl (auto-property) initializers,
// ReturnStmt, and the general call/constructor-argument path
// (`checkArgsAgainstParams`, ExprTypecheck_Calls.cpp) — the last of these is
// also what makes a record's PRIMARY-CONSTRUCTOR argument conform, since
// records in this language have no other way to receive a collection-literal
// field value. `Array<T>.Add()`'s argument is a SEPARATE, narrower gap: its
// checker (`checkBuiltinCollectionCallArgs`) never ran `checkAssignable` for
// `Array<T>.Add()` at all — "Array" was missing from the `Add` family name
// list `isItemPosition` gates on (the pre-existing header comment beside
// that list already named this exact gap: "`ICollection<T>.Add(item: T)` —
// the pos-0 `Add` block is name-gated to the concrete families").
//
// ONE ROOT CAUSE, MULTIPLE REPAIR POINTS — not one shared call site. The six
// collection-literal-typing sites above are already six independent
// call-site copies of the SAME `isCollectionLiteralAdmissibleTarget`
// admissibility gate (pre-existing, not introduced by this fix) — there is
// no single funnel point in this codebase collection literals all pass
// through, because LocalDecl/ReturnStmt/FieldDecl/etc. are different
// statement kinds with different surrounding logic. This fix does not add a
// SEVENTH hand-written copy of the element-walk: it factors the walk into
// ONE helper (`checkCollectionLiteralElementAssignability`) and calls that
// helper from each existing site — one authority, six callers, which is the
// discipline CLAUDE.md's "duplicated classification predicates" note asks
// for when a single call site genuinely is not available. `Array<T>.Add()`
// needed a real second repair (a one-line name-list addition, not a new
// predicate) because it does not go through any of the six collection-
// literal-typing sites at all — its argument is a plain identifier/call
// expression, not a collection literal.
//
// SCOPE, MEASURED AND FILED SEPARATELY (not fixed here):
//   - #4316: a plain `AssignmentStmt` (`outer := [...]`, not a declaration)
//     has NO admissibility gate or per-element check at all — a different,
//     larger gap than the six positions above (those all at least ran the
//     admissibility gate before this fix).
//   - #4315: an UNRELATED heap-use-after-free for a NAMED local crossing a
//     nested `do...end` block, declare-then-assign, from a call returning
//     an inline array of `string` or of a record containing a `string`
//     field — no interface involved; found while investigating a wrong-value
//     report a previous lane surfaced in passing.
//   - #4317: an UNRELATED codegen SIGSEGV for a genuinely CONFORMING record
//     (not class) stored into an interface-typed INLINE array element —
//     confirmed pre-existing (reproduces on the pre-#4310 compiler,
//     unaffected by this sema-only fix). This is why the "class element and
//     a record element" accept-side control below uses a HEAP `Array<T>`,
//     not an inline array, for the record — an inline-array-of-record
//     control would crash on #4317, not on anything this fix touches.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §5.1 ("... has a type that is not implicitly convertible to the target
// type (§5.1)") — the SAME diagnostic `checkAssignable` already produces for
// a plain variable declaration and an ordinary call argument (verified by
// the two "_control" rows below); this fix reuses that exact call rather
// than inventing a second diagnostic, so every position below asserts the
// identical text.
bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// Shared prelude: `IHolder` (one method), a NON-conforming `Widget3` (never
// lists `IHolder`), two DIFFERENT conforming classes (`Widget3Ok`/
// `Widget4Ok`), and a conforming record (`RecHolderOk`, §9.6 — a record may
// implement an interface that declares no settable member).
const std::string kPrelude =
    "module M; import System.Collections.Generic; "
    "interface IHolder method GetV(): string; end IHolder; "
    "class Widget3 "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3; "
    "class Widget3Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3Ok; "
    "class Widget4Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget4Ok; "
    "record RecHolderOk(V: string): IHolder "
    "  public method GetV(): string => V; "
    "end RecHolderOk; "
    "record HolderBox(Items: [1]IHolder); ";

JitResult run(const std::string& mainBody,
              const std::string& extraMember = "") {
    return runAot(kPrelude +
                  "class C public static method Main(): int " +
                  mainBody + " end Main; " + extraMember + " end C;");
}

}  // namespace

// ============================================================
// CONTROLS — the two positions checkAssignable already covered before this
// fix, proving the fix reuses the SAME authority rather than a new one.
// ============================================================

BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           plain_local_declaration_noconform_rejected_control_spec_5_1) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var h: IHolder := new Widget3(s); "
        "return h.GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           plain_call_argument_noconform_rejected_control_spec_5_1) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "return Foo(new Widget3(s));",
        /*extraMember=*/
        "public static method Foo(h: IHolder): int "
        "  return h.GetV().Length; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// REJECT ROWS — #4310's own repair. Each fails without the fix (compiles
// clean, then SIGSEGVs at the marked access) and rejects with it.
// ============================================================

// Case 3 (#4310's own numbering) / #4302's crash shape: an inline-array
// LITERAL initializer.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           inline_array_literal_local_decl_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [1]IHolder := [new Widget3(s)]; "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Case 4: a HEAP `Array<T>` literal, `var` LocalDecl.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           heap_array_literal_local_decl_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3(s); "
        "var arr: Array<IHolder> := [w]; "
        "return arr[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `LocalConstDecl` — a DIFFERENT AST node from `LocalDecl` (§21.1's runtime
// `const`); the LocalDecl fix does not reach it (measured: without a
// SEPARATE fix at this site the identical program still compiled clean).
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           local_const_heap_array_literal_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3(s); "
        "const arr: Array<IHolder> := [w]; "
        "return arr[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Case 5 / a narrower fix than the collection-literal-element check above:
// `Array<T>.Add()`'s argument — `checkBuiltinCollectionCallArgs` never ran
// `checkAssignable` for it at all ("Array" missing from the `Add`
// name-gated family list).
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           array_add_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3(s); "
        "using var arr := new Array<IHolder>(); "
        "arr.Add(w); "
        "return arr[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Nested: `[1]Array<IHolder>` — an inline array whose element is itself a
// heap-array collection literal. The per-element check RECURSES one level
// (the outer element is a nested `CollectionLiteralExpr`, not a `NewExpr`)
// rather than assuming the outer check alone covers it.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           nested_inline_array_of_heap_array_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3(s); "
        "var outer: [1]Array<IHolder> := [[w]]; "
        "return outer[0][0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
    // No accept-side control for the CONFORMING twin of this exact nested
    // shape: `[1]Array<IHolder> := [[w]]` with a conforming `w` hits a
    // PRE-EXISTING, unrelated codegen ICE ("internal: lowerExpr returned
    // null for local-decl inline-array initializer element — #1386"),
    // reproduced even with `[1]Array<int> := [[42]]` (no interface, no class
    // at all) — so that control cannot presently compile regardless of this
    // fix. The reject-side above is unaffected by #1386 (it never reaches
    // codegen).
}

// An interface element in a RECORD field's initializer — expressed via the
// record's PRIMARY CONSTRUCTOR call argument, the only way this language
// gives a record a collection-literal field value. Goes through
// `checkArgsAgainstParams` (ExprTypecheck_Calls.cpp), the general call/
// constructor-argument path — a DIFFERENT function from the LocalDecl one.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           record_primary_ctor_arg_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var box := new HolderBox([new Widget3(s)]); "
        "return box.Items[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A plain (non-property) FIELD explicit initializer.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           field_initializer_noconform_rejected_spec_5_1_11_2) {
    auto r = runAot(
        kPrelude +
        "class Holder public Items: [1]IHolder := [new Widget3(\"x\")]; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(); return h.Items[0].GetV().Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// An auto-PROPERTY initializer — the PropertyDecl sibling of the FieldDecl
// arm just above (a separate code arm in ExprTypecheck.cpp).
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           auto_property_initializer_noconform_rejected_spec_5_1_11_2) {
    auto r = runAot(
        kPrelude +
        "class Holder public property Items: [1]IHolder := "
        "  [new Widget3(\"x\")]; end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(); return h.Items[0].GetV().Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `ReturnStmt` — #4302's OWN exact repro shape: a function returning
// `[1]IHolder` from a collection literal, the caller assigning the result
// across a narrower `do...end` block before reading it.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           return_stmt_noconform_rejected_closes_4302_spec_5_1_11_2) {
    auto r = runAot(
        kPrelude +
        "class Helper public static method Wrap(s: string): [1]IHolder "
        "  return [new Widget3(s)]; end Wrap; end Helper; "
        "class C public static method Main(): int "
        "  var outer: [1]IHolder; "
        "  do var s := \"he\" + \"llo\"; outer := Helper.Wrap(s); end; "
        "  return outer[0].GetV().Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// ACCEPT-SIDE CONTROLS — must keep compiling and produce the correct value.
// A false reject here would be worse than the bug this fix closes.
// ============================================================

// A genuinely conforming class as a single inline-array element.
// NOTE: switched to `[1 owned]` (the spec-conformant form per §15.1.1 /
// #4336 / `569a680`): the non-owning `[1]IHolder` is a BORROW slot per §15.1
// and the `new Widget3Ok(s)` initializer is now rejected by #4351's sema
// fix. The owning form takes ownership of the fresh widget on construction
// and exercises the SAME inline-array element conformance path this test
// pins.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           conforming_single_element_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [1 owned]IHolder := [new Widget3Ok(s)]; "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// TWO DIFFERENT conforming implementors in one inline array.
// See note above: switched to `[2 owned]` for #4351.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           two_different_conforming_implementors_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [2 owned]IHolder := [new Widget3Ok(s), new Widget4Ok(\"world\")]; "
        "return outer[0].GetV().Length + outer[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// A nullable interface element defaulted to null (no initializer) — must
// stay accepted; this fix's element check only runs against an actual
// collection-LITERAL's elements, never against an absent initializer.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           nullable_interface_element_defaulted_null_accepted_control) {
    auto r = run(
        "var outer: [1]IHolder?; "
        "return (if outer[0] = null then 42 else 0);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// A class element and a RECORD element, both genuinely conforming, in the
// SAME heap `Array<IHolder>` (§9.6 permits a record to implement an
// interface with no settable member). Uses `Array<T>`, not `[N]T` — see the
// file-header note on #4317, an unrelated pre-existing codegen crash for a
// conforming record specifically at an INLINE-array interface element;
// heap `Array<T>` has no such gap (measured clean on both the pre- and
// post-#4310 compiler).
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           class_and_record_element_mixed_heap_array_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var arr: Array<IHolder> := "
        "  [new Widget3Ok(s), new RecHolderOk(\"world\")]; "
        "return arr[0].GetV().Length + arr[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// `Array<T>.Add()` with a genuinely conforming class — must stay accepted.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           array_add_conforming_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3Ok(s); "
        "using var arr := new Array<IHolder>(); "
        "arr.Add(w); "
        "return arr[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A conforming record field's primary-constructor argument — the accept
// twin of `record_primary_ctor_arg_noconform_rejected` above. Bound to a
// named local first (`items`), not passed as a literal temporary directly,
// because a record may not be constructed from a TEMPORARY bound to a
// borrow-carrying field (§9.7/§28.3 borrow-lifetime rule) — an unrelated,
// pre-existing rule this fix does not touch or need to route around.
BEZEL_TEST(collection_literal_element_interface_conformance_4310,
           record_primary_ctor_arg_conforming_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3Ok(s); "
        "var items: [1]IHolder := [w]; "
        "var box := new HolderBox(items); "
        "return box.Items[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
