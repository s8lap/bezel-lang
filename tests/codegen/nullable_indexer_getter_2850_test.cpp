// Spec-derived regression test for issue #2850.
//
// §7.2 (Nullable value types) / §23.4 (Indexers) / §23.5 (Restrictions), v0.240.
//
// §7.2 states, verbatim:
//
//     "A value type wrapped in `?` is an optional: it either holds a value or
//      `null`. `int?`, `Priority?`, and `DateTime?` are optional value types."
//
// and §23.5's restriction list names exactly one excluded declared type:
//
//     "Properties and indexers whose declared type is a move-only value type
//      (an owning inline array, §15.3) are not admitted."
//
// So `public property [i: int]: int?` IS an admitted declaration — `int?` is
// not move-only — and its getter body may legally yield `null` (§7.2's second
// alternative) or a bare `int` (§7.1: "A non-nullable reference type implicitly
// converts to its nullable form"; §20's common-type rule spells the same
// conversion for value types: "non-nullable `T` into `T?`"). The obligation is
// therefore entirely on codegen: the read must produce the tagged
// `{i8 hasValue, T}` optional the rest of the pipeline consumes. There is
// nothing here for sema to reject.
//
// SYMPTOM (measured on b0fa41a). An EXPRESSION-bodied indexer getter whose
// declared element type is a nullable VALUE type handed the caller the RAW body
// value instead of the optional. Two spellings, two different failures, because
// the expression-getter body is lowered by two different emitters:
//
//   A. NON-virtual — `Emitter::lowerIndexerGet`'s inline expression path
//      (src/codegen/LlvmBackend_Assign.cpp) inlines the body into the caller,
//      so the mismatch landed inside the caller's own IR:
//        `get => null;`   -> bezelc ABORTED:
//            Instructions.h:2719: PHINode::setIncomingValue: Assertion
//            `getType() == V->getType() && "All operands to PHI node must be
//            the same type as the PHI node!"' failed.  (core dumped)
//        `=> i + 5;`      -> bezelc ABORTED:
//            Instructions.h:942: checkGEPType: Assertion `Ty && "Invalid
//            GetElementPtrInst indices for type!"' failed.  (core dumped)
//      A compiler crash on a legal program: no diagnostic to act on.
//
//   B. `virtual` (and the interface/itable face) — `ensureIndexerGetterFn`
//      compiles the body as a standalone `T get(ptr this, idx…)` whose return
//      type is already `lowerType(pd->type)`, so the mismatch landed on its
//      `ret` and only the module verifier caught it:
//            Function return type does not match operand type of return inst!
//              ret ptr null
//             { i8, i32 }
//            bezelc: codegen error: IR verification failed
//
// ROOT CAUSE. #1168 gave the NAMED-property expression getter exactly this
// coercion (`wrapNullableGetterResult`, called from three sites in
// LlvmBackend_Exprs.cpp); the INDEXER spelling of the same body form never
// received it. Both emitters now apply it. `coerceNumericToType`, which both
// already called, cannot close the gap: it is a no-op on a pointer→struct or a
// scalar→struct pair.
//
// WHAT WAS ALREADY CORRECT, and is pinned below as a control: the BLOCK-bodied
// getter (`get => do return null; end;`) — it routes through
// `ensureIndexerBlockGetter`, whose `accessorReturnTypeNode_` wiring puts the
// body's `return` on the ordinary §21.7 return path — and every named-property
// form, virtual or not.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

// The raw stdout channel, for the multi-line programs below (runAotOut strips
// exactly one trailing terminator and is documented as the SINGLE-value helper).
std::string out(const bezel::test::JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPROS. Both ABORTED/failed the verifier pre-fix; a correct
// build reads the null optional and `?? 7` yields 7.
// ---------------------------------------------------------------------------

// #2850 symptom A / §7.2 + §23.4 — NON-virtual block-form indexer with an
// EXPRESSION getter accessor. Pre-fix: PHINode::setIncomingValue assert, core
// dumped.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           nonvirtual_expression_accessor_get_null_reads_as_null_7_2) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public property [i: int]: int? "
        "    get => null; "
        "    set => do _s := value ?? 0; end; "
        "  end; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(); return b[0] ?? 7; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 7);
}

// #2850 symptom B / §7.2 + §23.4 ("They participate in
// `virtual`/`override`/`sealed`/`abstract` like methods") — the SAME indexer
// marked `virtual` takes the vtable path, whose slot `ensureIndexerGetterFn`
// fills. Pre-fix: `ret ptr null` from a `{ i8, i32 }` function, rejected by the
// IR verifier.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           virtual_expression_accessor_get_null_reads_as_null_23_4) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int? "
        "    get => null; "
        "    set => do _s := value ?? 0; end; "
        "  end; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(); return b[0] ?? 7; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 7);
}

// #2850 / §23.4 ("expression-bodied") — the third spelling of the same body
// form, `property [i: int]: int? => null;`. Same inline emitter, same assert.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           expression_bodied_indexer_null_reads_as_null_23_4) {
    auto r = runAot(
        "module M; "
        "class B public property [i: int]: int? => null; end B; "
        "class C public static method Main(): int "
        "  using var b := new B(); return b[0] ?? 7; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 7);
}

// #2850 / §7.2 — the OTHER half of "it either holds a value or `null`": a body
// yielding a BARE non-nullable `int` into an `int?` element must be wrapped as
// `{i8 1, i32}`, not handed back raw. Pre-fix this was a SECOND crash shape
// (checkGEPType assert), so a fix that only special-cased the `null` literal
// would leave it. `b[1]` is `1 + 5`, so the `??` must NOT take its fallback.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           expression_body_bare_value_wraps_into_the_optional_7_2) {
    auto r = runAot(
        "module M; "
        "class B public property [i: int]: int? => i + 5; end B; "
        "class C public static method Main(): int "
        "  using var b := new B(); return b[1] ?? 99; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 6);
}

// #2850 / §11.2 + §23.4 ("Indexers may be declared in interfaces with the
// body-less form (write `readonly` before `property` to expose only a
// getter)") — the ITABLE face of `ensureIndexerGetterFn`, reached
// through an interface-typed receiver rather than a `virtual` slot. Both body
// shapes (a bare `null` and a bare value) get their own class, so each is a
// PLAIN body: a `(if … then null else v)` body would already be the tagged
// optional by §20's common-type rule ("A bare `null` branch … target-types to
// the nullable form of the other branches' common type") and could not observe
// this defect at all.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           interface_dispatched_nullable_indexer_11_2) {
    auto r = runAotOut(
        "module M; "
        "interface INullIdx readonly property [i: int]: int?; end INullIdx; "
        "class B: INullIdx public property [i: int]: int? => null; end B; "
        "class V: INullIdx public property [i: int]: int? => i * 2; end V; "
        "class C public static method Main(): int "
        "  using var b := new B(); using var v := new V(); "
        "  var q: INullIdx := b; "
        "  var w: INullIdx := v; "
        "  Console.WriteLine((q[0] ?? 7) * 100 + (w[3] ?? 9)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r == "706");
}

// #2850 / §7.2 — the ELEMENT-TYPE grid. §7.2 names `int?`, `Priority?` (an
// enum) and `DateTime?` (a record-like value type) as optional value types, so
// the coercion must be keyed on "is a nullable value type", not on a list of
// primitives. `decimal?` is included deliberately: it is the widest primitive
// payload. A category-blind fix passes all seven; a primitive-enumerating one
// would drop the enum and the record.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           nullable_element_type_grid_7_2) {
    auto r = out(runAot(
        "module M; "
        "enum Color Red, Green; end Color; "
        "record Pt(X: int, Y: int); "
        "class B public property [i: int]: bool? => null; end B; "
        "class D public property [i: int]: double? => null; end D; "
        "class E public property [i: int]: char? => null; end E; "
        "class F public property [i: int]: decimal? => null; end F; "
        "class G public property [i: int]: Color? => null; end G; "
        "class H public property [i: int]: Pt? => null; end H; "
        "class I public property [i: int]: long? => 7; end I; "
        "class C public static method Main(): int "
        "  using var b := new B(); using var d := new D(); "
        "  using var e := new E(); using var f := new F(); "
        "  using var g := new G(); using var h := new H(); "
        "  using var ii := new I(); "
        "  Console.WriteLine(b[0] ?? true); "
        "  Console.WriteLine(d[0] ?? 1.5); "
        "  Console.WriteLine(e[0] ?? 'z'); "
        "  Console.WriteLine(f[0] ?? 2.5m); "
        "  Console.WriteLine((g[0] ?? Color.Green) = Color.Green); "
        "  Console.WriteLine((h[0] ?? new Pt(3, 4)).X); "
        "  Console.WriteLine(ii[0] ?? 11); "
        "  return 0; "
        "end Main; end C;"));
    BEZEL_EXPECT_TRUE(r == "True\n1.5\nz\n2.5\nTrue\n3\n7\n");
}

// #2850 / §23.4 ("Inside a member of a class with its own indexer, `this[i]` is
// required") + §23.4 ("Records may declare read-only indexers") — the receiver
// axis: `this[i]` from inside the declaring class (both body shapes), and a
// RECORD receiver, which the inline path spills to a stack slot before binding
// `this`. Plain bodies throughout, for the §20 reason given above.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           this_index_and_record_receiver_23_4) {
    auto r = runAotOut(
        "module M; "
        "record R(N: int) "
        "  public property [i: int]: int? => N + i; "
        "end R; "
        "class B "
        "  public property [i: int]: int? => null; "
        "  public method ViaThis(): int => this[0] ?? 100; "
        "end B; "
        "class W "
        "  public property [i: int]: int? => i * 10; "
        "  public method ViaThis(): int => this[2] ?? 0; "
        "end W; "
        "class C public static method Main(): int "
        "  using var b := new B(); using var w := new W(); "
        "  var r := new R(7); "
        "  Console.WriteLine(b.ViaThis() * 10000 + w.ViaThis() * 100 "
        "                    + (r[1] ?? 66)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r == "1002008");
}

// #2850 / §14.1 (generics are monomorphized) + §7.10 — a GENERIC indexer whose
// element is `T?`. The coercion runs while the receiver's generic substitution
// is installed, so `T?` must resolve to the concrete `int?`.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           generic_nullable_element_indexer_14_1) {
    auto r = runAotOut(
        "module M; "
        "class Box<T> "
        "  public _v: T?; "
        "  public property [i: int]: T? => _v; "
        "end Box; "
        "class Box2<T> public property [i: int]: T? => null; end Box2; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(); using var b2 := new Box2<int>(); "
        "  Console.WriteLine((b[0] ?? 11) * 100 + (b2[0] ?? 12)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r == "1112");
}

// ---------------------------------------------------------------------------
// ACCEPT CONTROLS — the shapes that ALREADY worked and must be untouched. A
// fix that wrapped too eagerly would break every one of these.
// ---------------------------------------------------------------------------

// #2850 CONTROL / §23.4 — NON-nullable element types, the overwhelming majority
// of indexers: a value type, a `string`, a record, and a class-typed borrow. If
// the new coercion fired on any of these the read would return a struct where
// the caller expects a scalar/pointer.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           non_nullable_elements_are_unchanged_23_4) {
    auto r = out(runAot(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class K public V: int := 5; end K; "
        "class A public property [i: int]: int => i + 1; end A; "
        "class B public property [i: int]: string => \"s\"; end B; "
        "class F public property [i: int]: Pt => new Pt(i, i); end F; "
        "class G "
        "  public owned _k: K := new K(); "
        "  public property [i: int]: K => _k; "
        "end G; "
        "class C public static method Main(): int "
        "  using var a := new A(); using var b := new B(); "
        "  using var f := new F(); using var g := new G(); "
        "  Console.WriteLine(a[1]); "
        "  Console.WriteLine(b[0]); "
        "  Console.WriteLine(f[2].Y); "
        "  Console.WriteLine(g[0].V); "
        "  return 0; "
        "end Main; end C;"));
    BEZEL_EXPECT_TRUE(r == "2\ns\n2\n5\n");
}

// #2850 CONTROL / §7.1 + §8.6 — a nullable REFERENCE element (`string?`,
// `Object?`) is NOT a nullable value type: `isNullableValueTy` is false, so the
// body's `null` pointer must reach the caller as a plain null pointer. The
// `Object` element also pins #2761's box (the coercion runs AFTER boxForTarget
// and must not disturb it): `e[3] is int` stays true.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           nullable_reference_and_object_elements_are_unchanged_8_6) {
    auto r = out(runAot(
        "module M; "
        "class D public property [i: int]: string? => null; end D; "
        "class E public property [i: int]: Object => i; end E; "
        "class H public property [i: int]: Object? => null; end H; "
        "class C public static method Main(): int "
        "  using var d := new D(); using var e := new E(); using var h := new H(); "
        "  Console.WriteLine(d[0] ?? \"n\"); "
        "  Console.WriteLine(e[3] is int); "
        "  Console.WriteLine(h[0] = null); "
        "  return 0; "
        "end Main; end C;"));
    BEZEL_EXPECT_TRUE(r == "n\nTrue\nTrue\n");
}

// #2850 CONTROL / §23.3 + §22.9 — the BLOCK-bodied getter, which was already
// correct (its `return null;` runs the ordinary return path via
// `accessorReturnTypeNode_`). Pinned so a future rework of that path cannot
// silently regress the shape this fix's sibling emitter covers.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           block_bodied_getter_null_return_still_works_23_3) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public property [i: int]: int? "
        "    get => do return null; end; "
        "    set => do _s := value ?? 0; end; "
        "  end; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(); return b[0] ?? 7; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 7);
}

// #2850 CONTROL / §7.2 — a body that ALREADY yields the optional (`=> _f` over
// an `int?` field) must pass through untouched, not be double-wrapped.
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           already_optional_body_is_not_double_wrapped_7_2) {
    auto r = runAot(
        "module M; "
        "class D "
        "  public _f: int? := 42; "
        "  public property [i: int]: int? => _f; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d[0] ?? 98; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 42);
}

// #2850 / §28.9 — the ASan/LSan net over the fixed path. A nullable-VALUE
// optional is a plain aggregate (no runtime-managed payload), so a loop of
// reads must allocate nothing and free nothing; `runAotChecked` proves the fix
// introduced no leak and no over-release in either emitter (the loop drives the
// non-virtual inline path and the `virtual` vtable path in the same program).
BEZEL_TEST(codegen_nullable_indexer_getter_2850,
           looped_reads_are_allocation_neutral_28_9) {
    auto r = runAotChecked(
        "module M; "
        "class B public property [i: int]: int? => null; end B; "
        "class V public virtual property [i: int]: int? => i; end V; "
        "class C public static method Main(): int "
        "  using var b := new B(); using var v := new V(); "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    t := t + (b[i] ?? 100) + (v[i] ?? 200); "
        "    i := i + 1; "
        "  end while; "
        "  if t <> 2190 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}
