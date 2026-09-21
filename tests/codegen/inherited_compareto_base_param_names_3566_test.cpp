// fix #3566 / stdlib §4.17 "SortedDictionary<K, V>" + spec §2.8.3 rule 4
// "Identity is one type at runtime, too" + §14.1 *Instantiation and its site*,
// v0.306.
//
// Family I1 (docs/proposals/defect-campaign-2026-09.md §2): a substitution
// installed under the WRONG SET OF PARAMETER NAMES. `fix #3530` made the
// class-comparator thunk (`bz$classcmp$<C>`) call the MONOMORPHIZED `CompareTo`
// instead of the placeholder template, installing the substitution from
// `genericParamNames(elementType->declaration)` zipped against that TypeRef's
// `genericArgs`. That is the right map ONLY when the `CompareTo` it found is
// declared by the element type itself. `findMethodByName` walks up the base
// chain, so the body may read the BASE's parameter names, which need not be the
// derived class's:
//
//     class GBase<T>: IComparable<GBase<T>>   // body reads `T`
//     class Derived<U>: GBase<U>              // subst installed as {U -> long}
//
// `{U -> i64}` leaves `T` unbound, so the "specialization" lowered `Payload: T`
// at the i32 placeholder and came out INSTRUCTION-IDENTICAL to the template.
// #3530 silently did nothing for that shape, and #3566 then DECLINED it
// outright (`if (ms->parent != ty->declaration) return nullptr;`) rather than
// emit a body that was still effectively a template.
//
// ⚠ WHY THIS IS NOT AN ORDERING NICETY. stdlib §4.17 makes `K`'s
// `IComparable<K>.CompareTo` both the ordering authority — "Ordering is defined
// by `K`'s `IComparable<K>.CompareTo`" — and the KEY IDENTITY — "Two keys are
// the same key exactly when `CompareTo` returns `0`". A comparator answering
// from the wrong field offsets therefore decides ITERATION ORDER *and* MAP
// MEMBERSHIP. spec §2.8.3 rule 4 makes the monomorphization of a constructed
// generic type one type at runtime; a body lowered at the placeholder is a
// second, wrong one.
//
// MEASURED on integration HEAD 00e10c0e with the issue's own repro: keys
// inserted 3, 1, 2 printed `first=3 last=2 count=3` — exactly insertion order,
// i.e. the comparator answered "equal"/garbage for every pair. Silent wrong
// answer, rc=0, no diagnostic at any severity.
//
// THE FIX composes the derived class's arguments THROUGH the base-type
// reference chain to the declaring base and installs the map keyed by that
// base's parameter names — reusing `computeAncestorGenericSubst`, the walk
// `fieldAddr`'s inherited-field branch (#2840/#585) already performs, rather
// than adding a second spelling of it. Because that walk re-derives each level
// POSITIONALLY from its base clause, a renamed parameter resolves exactly as an
// identically-spelled one does.
//
// ⚠ Reachability: `Array<Derived<long>>.Sort()` CANNOT reach this — sema
// rejects it with "Derived<long> does not implement IComparable<Derived<long>>".
// It is reachable through `SortedDictionary`, because nothing currently checks
// the §4.17 `where K: IComparable<K>` constraint for a builtin generic (#3490).
// The rows below therefore go through `SortedDictionary`, as the issue's repro
// does.
//
// ⚠ ONE OF THE ISSUE'S TWO DISCRIMINATORS NO LONGER HOLDS, and it is recorded
// here rather than trusted. #3566 states that renaming the derived parameter to
// MATCH the base (`class Derived<T>: GBase<T>`) gives the correct answer —
// measured true on the #3530 tree it was filed against, because the flat
// `{T -> long}` map happened to be right for that spelling. It is FALSE on
// integration HEAD 00e10c0e: #3566's own mitigation declines EVERY inherited
// `CompareTo` (`if (ms->parent != ty->declaration) return nullptr;`) regardless
// of parameter spelling, so the matching-name shape now calls the placeholder
// template too and prints `first=3 last=2 count=3`. Measured on the revert.
// It is therefore a BUG ROW below, not an accept control — writing it as a
// control would have shipped a row that passes on both sides of the fix.
//
// The issue's OTHER discriminator does still hold and IS an accept control:
// `<int>` instead of `<long>` is correct pre-fix, because the i32 placeholder
// layout coincides with the real one (measured `first=1 last=3 count=3` on the
// revert). So is the non-inherited `CompareTo` #3530 already fixed.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// `GBase<T>` carries a `T`-typed `Payload` BEFORE the `int` Key, so a body
// lowered at the i32 placeholder reads `Key` at the wrong offset whenever `T`
// is wider than `int`. That is the whole mechanism, in one class.
const char* kGBase =
    "class GBase<T>: IComparable<GBase<T>> "
    "  public Payload: T; "
    "  public Key: int := 0; "
    "  public constructor(p: T, k: int) Payload := p; Key := k; "
    "  end constructor; "
    "  public method CompareTo(other: GBase<T>): int "
    "    return Key - other.Key; "
    "  end CompareTo; "
    "end GBase; ";

}  // namespace

// ===========================================================================
// THE BUG, the issue's repro verbatim: base-declared `CompareTo` x RENAMED
// derived parameter x a type argument whose layout differs from the i32
// placeholder. Keys are inserted 3, 1, 2; a working comparator orders them
// 1, 2, 3.
// ===========================================================================
BEZEL_TEST(inherited_compareto_3566, renamed_param_base_compareto_spec_4_17) {
    auto out = runAotOut(
        std::string("module M; ") + kGBase +
        "class Derived<U>: GBase<U> "
        "  public constructor(p: U, k: int) base(p, k); end constructor; "
        "end Derived; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new Derived<long>(10L, 3); "
        "    using var b := new Derived<long>(20L, 1); "
        "    using var c := new Derived<long>(30L, 2); "
        "    var d := new SortedDictionary<Derived<long>, int>(); "
        "    d.Add(a, 30); d.Add(b, 10); d.Add(c, 20); "
        "    Console.WriteLine($\"first={d.FirstKey().Key} "
        "last={d.LastKey().Key} count={d.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    // Pre-fix: "first=3 last=2 count=3" (insertion order).
    BEZEL_EXPECT_EQ(out, std::string("first=1 last=3 count=3"));
}

// The same shape with the PAYLOAD read back, so a body reading the right Key
// offset by luck could not pass. `Payload` is the `long` whose width is what
// moves `Key`; the first key (Key=1) carries 20.
BEZEL_TEST(inherited_compareto_3566, renamed_param_payload_round_trip_spec_2_8_3) {
    auto out = runAotOut(
        std::string("module M; ") + kGBase +
        "class Derived<U>: GBase<U> "
        "  public constructor(p: U, k: int) base(p, k); end constructor; "
        "end Derived; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new Derived<long>(5000000010L, 3); "
        "    using var b := new Derived<long>(5000000020L, 1); "
        "    using var c := new Derived<long>(5000000030L, 2); "
        "    var d := new SortedDictionary<Derived<long>, int>(); "
        "    d.Add(a, 30); d.Add(b, 10); d.Add(c, 20); "
        "    Console.WriteLine($\"k={d.FirstKey().Key} "
        "p={d.FirstKey().Payload}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(out, std::string("k=1 p=5000000020"));
}

// A TWO-LEVEL chain with a rename at every level (`GLeaf<W>: GMid<V>:
// GRoot<T>`), which a single-level base lookup would not resolve: the walk has
// to compose `W -> V -> T` through two base clauses. This is the row that
// distinguishes "walk the chain" from "look at the immediate base".
BEZEL_TEST(inherited_compareto_3566, two_level_rename_chain_spec_14_1) {
    auto out = runAotOut(
        "module M; "
        "class GRoot<T>: IComparable<GRoot<T>> "
        "  public Payload: T; "
        "  public Key: int := 0; "
        "  public constructor(p: T, k: int) Payload := p; Key := k; "
        "  end constructor; "
        "  public method CompareTo(other: GRoot<T>): int "
        "    return Key - other.Key; "
        "  end CompareTo; "
        "end GRoot; "
        "class GMid<V>: GRoot<V> "
        "  public constructor(p: V, k: int) base(p, k); end constructor; "
        "end GMid; "
        "class GLeaf<W>: GMid<W> "
        "  public constructor(p: W, k: int) base(p, k); end constructor; "
        "end GLeaf; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new GLeaf<long>(10L, 3); "
        "    using var b := new GLeaf<long>(20L, 1); "
        "    using var c := new GLeaf<long>(30L, 2); "
        "    var d := new SortedDictionary<GLeaf<long>, int>(); "
        "    d.Add(a, 30); d.Add(b, 10); d.Add(c, 20); "
        "    Console.WriteLine($\"first={d.FirstKey().Key} "
        "last={d.LastKey().Key} count={d.Count} pay={d.FirstKey().Payload}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(out,
                    std::string("first=1 last=3 count=3 pay=20"));
}

// ===========================================================================
// BUG ROW — the shape the issue lists as a working DISCRIMINATOR and which is
// NOT one on HEAD. `class Derived<T>: GBase<T>` spells the base's parameter
// identically, so #3530's flat map was right for it; #3566's blanket decline
// then broke it anyway. Measured on the revert: `first=3 last=2 count=3`.
// Keeping it as a control would have been a row that cannot fail.
// ===========================================================================
BEZEL_TEST(inherited_compareto_3566, matching_param_name_also_declined_spec_4_17) {
    auto out = runAotOut(
        std::string("module M; ") + kGBase +
        "class Derived<T>: GBase<T> "
        "  public constructor(p: T, k: int) base(p, k); end constructor; "
        "end Derived; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new Derived<long>(10L, 3); "
        "    using var b := new Derived<long>(20L, 1); "
        "    using var c := new Derived<long>(30L, 2); "
        "    var d := new SortedDictionary<Derived<long>, int>(); "
        "    d.Add(a, 30); d.Add(b, 10); d.Add(c, 20); "
        "    Console.WriteLine($\"first={d.FirstKey().Key} "
        "last={d.LastKey().Key} count={d.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(out, std::string("first=1 last=3 count=3"));
}

// ===========================================================================
// THE SECOND SHAPE — a NON-generic class over a generic base. The caller's
// gate (`genericInst`) asks whether the ELEMENT type is a constructed generic,
// which is the right question only while the `CompareTo` is declared by that
// element type. `class DPlain: GBase<long>` has no parameters of its own, so
// the gate was false and the placeholder template was called — the layout that
// matters belongs to `GBase<long>`, not to `DPlain`. Found by adversarial
// review of the first fix, not by the issue; measured `first=3 last=2 count=3`
// before the gate was widened.
// ===========================================================================
BEZEL_TEST(inherited_compareto_3566, non_generic_derived_over_generic_base_spec_4_17) {
    auto out = runAotOut(
        std::string("module M; ") + kGBase +
        "class DPlain: GBase<long> "
        "  public constructor(p: long, k: int) base(p, k); end constructor; "
        "end DPlain; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new DPlain(10L, 3); "
        "    using var b := new DPlain(20L, 1); "
        "    using var c := new DPlain(30L, 2); "
        "    var d := new SortedDictionary<DPlain, int>(); "
        "    d.Add(a, 30); d.Add(b, 10); d.Add(c, 20); "
        "    Console.WriteLine($\"first={d.FirstKey().Key} "
        "last={d.LastKey().Key} count={d.Count} pay={d.FirstKey().Payload}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(out, std::string("first=1 last=3 count=3 pay=20"));
}

// ===========================================================================
// ACCEPT CONTROLS — measured correct on the REVERT as well as with the fix, so
// they pin that it narrowed nothing.
// ===========================================================================

// The issue's surviving discriminator: keep the rename but use `<int>`, where
// the i32 placeholder layout coincides with the real one. Correct on both
// sides (measured `first=1 last=3 count=3` on the revert).
BEZEL_TEST(inherited_compareto_3566, control_int_argument_placeholder_coincides) {
    auto out = runAotOut(
        std::string("module M; ") + kGBase +
        "class Derived<U>: GBase<U> "
        "  public constructor(p: U, k: int) base(p, k); end constructor; "
        "end Derived; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new Derived<int>(10, 3); "
        "    using var b := new Derived<int>(20, 1); "
        "    using var c := new Derived<int>(30, 2); "
        "    var d := new SortedDictionary<Derived<int>, int>(); "
        "    d.Add(a, 30); d.Add(b, 10); d.Add(c, 20); "
        "    Console.WriteLine($\"first={d.FirstKey().Key} "
        "last={d.LastKey().Key} count={d.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(out, std::string("first=1 last=3 count=3"));
}

// #3530's own case — `CompareTo` declared BY the element type — must be
// untouched: the fix only changes the `ms->parent != ty->declaration` branch.
BEZEL_TEST(inherited_compareto_3566, control_self_declared_compareto_3530) {
    auto out = runAotOut(
        "module M; "
        "class GSelf<T>: IComparable<GSelf<T>> "
        "  public Payload: T; "
        "  public Key: int := 0; "
        "  public constructor(p: T, k: int) Payload := p; Key := k; "
        "  end constructor; "
        "  public method CompareTo(other: GSelf<T>): int "
        "    return Key - other.Key; "
        "  end CompareTo; "
        "end GSelf; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new GSelf<long>(10L, 3); "
        "    using var b := new GSelf<long>(20L, 1); "
        "    using var c := new GSelf<long>(30L, 2); "
        "    var d := new SortedDictionary<GSelf<long>, int>(); "
        "    d.Add(a, 30); d.Add(b, 10); d.Add(c, 20); "
        "    Console.WriteLine($\"first={d.FirstKey().Key} "
        "last={d.LastKey().Key} count={d.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(out, std::string("first=1 last=3 count=3"));
}
