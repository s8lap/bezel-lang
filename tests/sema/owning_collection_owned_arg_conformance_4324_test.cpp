// Spec-derived regression test for issue #4324.
//
// §5.1 / §11.2 (Implicit Conversions / Conformance) / stdlib §4.4–§4.18
// (collections): a builtin owning-collection mutator that declares an
// `owned item: T` parameter must still verify that the argument type
// conforms to T — the §22.3 ownership transfer is orthogonal to the §5.1/
// §11.2 conformance check. Previously the modifier guard in
// `checkBuiltinCollectionCallArgs` skipped ALL checks (nullability AND
// conformance) for any argument carrying a modifier, including the `owned`
// marker on every owning-family mutator. The conformance check was silently
// skipped, and a class that did not implement the element interface was
// accepted and reached codegen, where the null-itable dispatch from
// `arr[0].GetV()` caused a SIGSEGV (exit 139).
//
// The general call path (`checkArgsAgainstParams`) already proves the pattern
// is sound: it runs `checkAssignable` regardless of modifier and lets the
// ownership pass separately enforce §22.3. The builtin-collection helper now
// mirrors this for the owning families.
//
// fix #4324 (§5.1/§11.2, v0.298)

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "support/aot_support.hpp"       // runAot — AOT run oracle
#include "test_harness.hpp"

#include <initializer_list>
#include <string>

namespace {

using bezel::test::runAot;

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §5.1 "not implicitly convertible" diagnostic fragment, shared by the
// user-method path and (after fix #4324) the owning builtin-collection path.
constexpr const char* kConformanceDiag =
    "has a type that is not implicitly convertible to the target type";

}  // namespace

// #4324 / stdlib §4.4: the issue's repro — `OwningArray<IHolder>.Add(
// owned item: IHolder)` with a `Widget3` that does not list IHolder.
// Without the fix this compiled with zero diagnostics (the modifier guard
// skipped the conformance check) and SEGV'd at runtime on `arr[0].GetV()`
// (exit 139); with the fix it is a clean sema reject.
BEZEL_TEST(sema_owning_collection_owned_arg_conformance_4324,
           owning_array_add_rejects_non_conforming_arg) {
    auto r = parseSemaAll(
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "class Widget3 "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "  public method GetV(): string return V; end GetV; "
        "end Widget3; "
        "class C "
        "  public static method Main(): int "
        "    using var arr := new OwningArray<IHolder>(); "
        "    arr.Add(new Widget3(\"x\")); "
        "    return arr[0].GetV().Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kConformanceDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "OwningArray<T>.Add"));
}

// #4324 / stdlib §4.5: OwningSet<T>.Add(owned item: T) — same gap.
BEZEL_TEST(sema_owning_collection_owned_arg_conformance_4324,
           owning_set_add_rejects_non_conforming_arg) {
    auto r = parseSemaAll(
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "class Widget3 "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "end Widget3; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new OwningSet<IHolder>(); "
        "    s.Add(new Widget3(\"x\")); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kConformanceDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "OwningSet<T>.Add"));
}

// #4324 / stdlib §4.6: OwningLinkedList<T>.AddFirst(owned item: T) — same gap.
BEZEL_TEST(sema_owning_collection_owned_arg_conformance_4324,
           owning_linkedlist_addfirst_rejects_non_conforming_arg) {
    auto r = parseSemaAll(
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "class Widget3 "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "end Widget3; "
        "class C "
        "  public static method Main(): int "
        "    using var ll := new OwningLinkedList<IHolder>(); "
        "    ll.AddFirst(new Widget3(\"x\")); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kConformanceDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "OwningLinkedList<T>.AddFirst"));
}

// #4324 / stdlib §4.7: OwningStack<T>.Push(owned item: T) — same gap.
BEZEL_TEST(sema_owning_collection_owned_arg_conformance_4324,
           owning_stack_push_rejects_non_conforming_arg) {
    auto r = parseSemaAll(
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "class Widget3 "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "end Widget3; "
        "class C "
        "  public static method Main(): int "
        "    using var st := new OwningStack<IHolder>(); "
        "    st.Push(new Widget3(\"x\")); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kConformanceDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "OwningStack<T>.Push"));
}

// #4324 / stdlib §4.8: OwningQueue<T>.Enqueue(owned item: T) — same gap.
BEZEL_TEST(sema_owning_collection_owned_arg_conformance_4324,
           owning_queue_enqueue_rejects_non_conforming_arg) {
    auto r = parseSemaAll(
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "class Widget3 "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "end Widget3; "
        "class C "
        "  public static method Main(): int "
        "    using var q := new OwningQueue<IHolder>(); "
        "    q.Enqueue(new Widget3(\"x\")); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kConformanceDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "OwningQueue<T>.Enqueue"));
}

// #4324 / stdlib §4.9: OwningDictionary<K,V>.Add(key: K, owned value: V)
// — the key slot (pos 0) is non-owned and was already covered; the owned
// value slot (pos 1) is the gap.
BEZEL_TEST(sema_owning_collection_owned_arg_conformance_4324,
           owning_dictionary_add_rejects_non_conforming_value) {
    auto r = parseSemaAll(
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "class Widget3 "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "end Widget3; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new OwningDictionary<string, IHolder>(); "
        "    d.Add(\"key\", new Widget3(\"x\")); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kConformanceDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "OwningDictionary<T>.Add"));
}

// Positive guard: a CONFORMING type on an owning collection mutator must
// still compile (no false rejection). Uses runAot to verify end-to-end.
BEZEL_TEST(sema_owning_collection_owned_arg_conformance_4324,
           owning_array_add_accepts_conforming_type) {
    auto r = runAot(
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "class Widget3Ok: IHolder "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "  public method GetV(): string return V; end GetV; "
        "end Widget3Ok; "
        "class C "
        "  public static method Main(): int "
        "    using var arr := new OwningArray<IHolder>(); "
        "    arr.Add(new Widget3Ok(\"hello\")); "
        "    return arr[0].GetV().Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
