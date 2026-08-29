// Spec-derived regression tests for #3530 (v0.283): `Array<C<A>>.Sort()` when
// `C<T>` is a GENERIC class whose fields are laid out by the type argument.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §4.4 (v0.283) declares
//     public method Sort();                    // in place
// on `Array<T>` under the note "Natural order; admitted only where
// `T: IComparable<T>`", and spec §14.2's *Conditionally-admitted members*
// register carries the row `Array<T>.Sort(), Array<T>.Sorted(),
// OwningArray<T>.Sort()` | `T` conforms to `IComparable<T>`.  So for
// `class Box<T>: IComparable<Box<T>>`, `Array<Box<long>>.Sort()` is admitted
// and must order the elements by the user's `CompareTo` — in place.
//
// Spec §14.1 admits exactly this declaration shape ("Classes, interfaces,
// records, and methods may be generic", with `interface IComparable<T>` as the
// section's own worked example), and §2.8.3 rule 3 fixes the identity a
// constructed instantiation has: "a constructed generic type is identified by
// its generic's identity plus the identities of its arguments".
//
// THE DEFECT (measured on e57a1eb, the parent of this fix).  The comparator
// thunk `bz$classcmp$<C>` that `Array<T>.Sort()` hands to `bz_array_sort`
// called `methodFns_[CompareTo]` — which for a generic class is the
// UNSPECIALIZED TEMPLATE, the body emitted with every type parameter at its
// i32 placeholder lowering.  The emitter documents that template as "never the
// call target — every call binds the concrete substitution and routes to a
// specialization" (`emittingGenericTemplateBody_`), and the comparator was the
// one caller that did not.  For `class Box<T>` with `Lead: T; V: int` and
// T = long, the template loads `V` at +0x10 (where it sits when `Lead` is the
// i32 placeholder) while the instantiation's own `Box$CompareTo$spec` loads it
// at +0x18 — so `CompareTo` compared the low half of `Lead` and `Sort()`
// returned the array in INSERTION ORDER with nothing diagnosed.  A silent
// violation of stdlib §4.4's "in place" natural-order sort.
//
// Second half, from the same root: `bz$classcmp$` carried only the type's
// module-qualified name (#3512 / #3529), not §2.8.3 rule 3's "identities of its
// arguments", so `Array<Box<int>>` and `Array<Box<long>>` in ONE program shared
// ONE thunk — whichever instantiation reached the emitter first decided both.
//
// WHY THE OBSERVABLE IS stdout AND NOT AN EXIT CODE.  A process exit status is
// masked to 8 bits, and the point of these cases is the ORDER of the elements,
// so each program PRINTS the post-sort sequence and the assertion is on that
// string.  "it compiled" and "exit 0" are not oracles for a silently unsorted
// array — the defect returned 0 from every one of these programs.
//
// ⚠ AND THE stdout ASSERTION IS THE *ONLY* ORACLE THESE ROWS HAVE.  An earlier
// revision of this header claimed generated-code AddressSanitizer would catch
// the defect as an "out-of-bounds / wrong-object read"; that is FALSE and worth
// stating so no later reader trusts the wrong net.  The template read `V` at
// +0x10 instead of +0x18 — that is the low half of the SAME object's `Lead`
// field, an initialized, in-bounds read of the same allocation, and ASan is
// blind to it by construction.  `runAotChecked` is kept on the defect rows for
// a different and real reason: #3530 makes the compiler EMIT a monomorphized
// `CompareTo` body per instantiation where it previously emitted none, so
// LeakSanitizer is the net for an allocation that new emission might strand.
// If a row here starts failing, the value in the message is the evidence; a
// clean sanitizer run proves nothing about the ordering.
//
// CONTROLS.  The three control cases pin the halves the fix must not disturb:
// a NON-generic class element (the pre-#3530 path, byte-identical mangling), an
// int element (the `bz$intcmp` arm), and a generic class whose `CompareTo` is
// VIRTUAL — that arm dispatched through the per-instantiation `vt$<C>$specN`
// and was already correct, which is why only a non-virtual `CompareTo` was
// affected.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {
// The reported shape: a type-parameter-typed field AHEAD of the int the
// comparison reads, so the template's placeholder layout and the
// instantiation's real layout disagree about where `V` lives.
const char* BOX_LONG =
    "module M; "
    "class Box<T>: IComparable<Box<T>> "
    "  public Lead: T; "
    "  public V: int := 0; "
    "  public constructor(lead: T, v: int) Lead := lead; V := v; end constructor; "
    "  public method CompareTo(other: Box<T>): int return V - other.V; end CompareTo; "
    "end Box; ";

// Generated-code ASan/LSan: a comparator reading a field at the wrong offset is
// an out-of-bounds / wrong-object read, which is exactly what this net catches
// at the access rather than as a value coincidence.
std::string runCheckedOut(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}
}  // namespace

// #3530 / stdlib §4.4 + spec §14.1 / §14.2 (v0.283): the reported repro.  Four
// elements inserted 3,1,4,2; `Sort()` must leave them 1,2,3,4.  The defect
// printed the insertion order 3142 unchanged.
BEZEL_TEST(array_sort_generic_class_elem_3530, sort_generic_class_element_orders_by_compareto) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        std::string(BOX_LONG) +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Box<long>(7, 3); "
        "    using var b := new Box<long>(8, 1); "
        "    using var c := new Box<long>(9, 4); "
        "    using var d := new Box<long>(6, 2); "
        "    var arr := new Array<Box<long>>(); "
        "    arr.Add(a); arr.Add(b); arr.Add(c); arr.Add(d); "
        "    arr.Sort(); "
        "    Console.WriteLine($\"{arr[0].V}{arr[1].V}{arr[2].V}{arr[3].V}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        std::string("1234"));
}

// #3530 / stdlib §4.4: `Sorted()` shares the same `bz$classcmp$` thunk (it was
// folded onto one builder by `narrow #3488`), so it carried the identical
// defect.  §4.4 fixes what it must do: "the past-participle form (`Reversed`,
// `Sorted`) leaves the receiver unchanged and returns a fresh `owned Array<T>`
// with the elements in the new order" — so BOTH halves are asserted: the copy
// is sorted AND the receiver is untouched.
BEZEL_TEST(array_sort_generic_class_elem_3530, sorted_generic_class_element_orders_and_leaves_receiver) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        std::string(BOX_LONG) +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Box<long>(7, 3); "
        "    using var b := new Box<long>(8, 1); "
        "    using var c := new Box<long>(9, 2); "
        "    var arr := new Array<Box<long>>(); "
        "    arr.Add(a); arr.Add(b); arr.Add(c); "
        "    using var s := arr.Sorted(); "
        "    Console.WriteLine($\"{s[0].V}{s[1].V}{s[2].V}-{arr[0].V}{arr[1].V}{arr[2].V}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        std::string("123-312"));
}

// #3530 second half / spec §2.8.3 rule 3 (v0.283): TWO instantiations of one
// generic class in ONE program.  The pre-fix thunk name carried no type
// arguments, so `Box<int>` and `Box<long>` shared one `bz$classcmp$M.Box` and
// whichever reached the emitter first decided both.  `Box<int>` happens to
// agree with the placeholder layout, so it is the instantiation that sorted
// correctly before the fix — asserting both in one program is what makes the
// collapse visible.
BEZEL_TEST(array_sort_generic_class_elem_3530, two_instantiations_get_their_own_comparator) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        std::string(BOX_LONG) +
        "class C "
        "  public static method Main(): int "
        "    using var ia := new Box<int>(7, 3); "
        "    using var ib := new Box<int>(8, 1); "
        "    using var ic := new Box<int>(9, 2); "
        "    using var la := new Box<long>(7, 3); "
        "    using var lb := new Box<long>(8, 1); "
        "    using var lc := new Box<long>(9, 2); "
        "    var ai := new Array<Box<int>>(); "
        "    ai.Add(ia); ai.Add(ib); ai.Add(ic); "
        "    var al := new Array<Box<long>>(); "
        "    al.Add(la); al.Add(lb); al.Add(lc); "
        "    ai.Sort(); al.Sort(); "
        "    Console.WriteLine($\"{ai[0].V}{ai[1].V}{ai[2].V}-{al[0].V}{al[1].V}{al[2].V}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        std::string("123-123"));
}

// #3530 / spec §14.1: a type argument WIDER than the placeholder is not the
// only disagreeing shape — a `string` (reference) type argument puts a pointer
// where the i32 placeholder sits, which shifts `V` by four bytes just the same.
BEZEL_TEST(array_sort_generic_class_elem_3530, reference_type_argument_also_orders_correctly) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        std::string(BOX_LONG) +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Box<string>(\"x\", 3); "
        "    using var b := new Box<string>(\"y\", 1); "
        "    using var c := new Box<string>(\"z\", 2); "
        "    var arr := new Array<Box<string>>(); "
        "    arr.Add(a); arr.Add(b); arr.Add(c); "
        "    arr.Sort(); "
        "    Console.WriteLine($\"{arr[0].V}{arr[1].V}{arr[2].V}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        std::string("123"));
}

// #3530 / spec §14.1 (v0.283): the element's instantiation is decided by an
// ENCLOSING generic — `Holder<T>` sorting an `Array<Box<T>>`, instantiated at
// both `long` and `int` in one program.  The thunk's identity has to be keyed
// on the argument COMPOSED THROUGH the active outer substitution, not on the
// raw `T`: a bare parameter's structural key is the same for every binding, so
// keying on it would give both `Holder` instantiations ONE thunk.  Pre-fix
// (e57a1eb) this printed `long=312 int=123` — the placeholder template, right
// only where the placeholder layout happens to suit.  The emitted symbols are
// the direct evidence: `bz$classcmp$M.Box$long`, `$int`, and `$T` (the dead
// template body's) are three distinct thunks.
BEZEL_TEST(array_sort_generic_class_elem_3530, enclosing_generic_binds_the_element_instantiation) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        std::string(BOX_LONG) +
        "class Holder<T> "
        "  public method SortThem(lo: Box<T>, hi: Box<T>, mid: Box<T>): int "
        "    var arr := new Array<Box<T>>(); "
        "    arr.Add(lo); arr.Add(hi); arr.Add(mid); "
        "    arr.Sort(); "
        "    return arr[0].V * 100 + arr[1].V * 10 + arr[2].V; "
        "  end SortThem; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var la := new Box<long>(7, 3); "
        "    using var lb := new Box<long>(8, 1); "
        "    using var lc := new Box<long>(9, 2); "
        "    using var ia := new Box<int>(7, 3); "
        "    using var ib := new Box<int>(8, 1); "
        "    using var ic := new Box<int>(9, 2); "
        "    using var hl := new Holder<long>(); "
        "    using var hi := new Holder<int>(); "
        "    Console.WriteLine($\"long={hl.SortThem(la, lb, lc)} int={hi.SortThem(ia, ib, ic)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        std::string("long=123 int=123"));
}

// #3530 / spec §14.1 + §2.8.3 rule 3 (v0.283): the same shape with a
// CONSTRUCTED type argument — `Array<Box<Pair<T>>>` for a generic RECORD
// `Pair<T>`, so the argument that must be composed is `Pair<T>`, not a bare
// parameter.  This row is why the composition is `substituteTypeRefLocal` and
// not a shallow resolve: a shallow one returns its argument unchanged as soon
// as the argument has a `declaration`, so `Pair<T>` keys identically at every
// binding of `T` while `Pair<long>` (16 bytes) and `Pair<int>` (8 bytes) put
// `V` at different offsets.  MEASURED: pre-#3530 `long=312 int=312` (both wrong,
// the placeholder template); with a shallow resolve `long=312 int=123` (ONE
// thunk `bz$classcmp$M.Box$Pair_T`, so one instantiation silently inherits the
// other's body — the collapse class #2192/#3020 record); composed,
// `long=123 int=123` with three thunks `$Pair_long`, `$Pair_int`, `$Pair_T`.
// The record element is a §14.1 declaration form ("record Pair<TFirst,
// TSecond>(First: TFirst, Second: TSecond)" is the section's own example).
BEZEL_TEST(array_sort_generic_class_elem_3530, constructed_type_argument_keys_per_binding) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; "
        "record Pair<T>(A: T, B: int); "
        "class Box<T>: IComparable<Box<T>> "
        "  public Lead: T; "
        "  public V: int := 0; "
        "  public constructor(lead: T, v: int) Lead := lead; V := v; end constructor; "
        "  public method CompareTo(other: Box<T>): int return V - other.V; end CompareTo; "
        "end Box; "
        "class Holder<T> "
        "  public method Run(x: Box<Pair<T>>, y: Box<Pair<T>>, z: Box<Pair<T>>): int "
        "    var arr := new Array<Box<Pair<T>>>(); "
        "    arr.Add(x); arr.Add(y); arr.Add(z); "
        "    arr.Sort(); "
        "    return arr[0].V * 100 + arr[1].V * 10 + arr[2].V; "
        "  end Run; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var la := new Box<Pair<long>>(new Pair<long>(1, 1), 3); "
        "    using var lb := new Box<Pair<long>>(new Pair<long>(2, 2), 1); "
        "    using var lc := new Box<Pair<long>>(new Pair<long>(3, 3), 2); "
        "    using var ia := new Box<Pair<int>>(new Pair<int>(1, 1), 3); "
        "    using var ib := new Box<Pair<int>>(new Pair<int>(2, 2), 1); "
        "    using var ic := new Box<Pair<int>>(new Pair<int>(3, 3), 2); "
        "    using var hl := new Holder<long>(); "
        "    using var hi := new Holder<int>(); "
        "    Console.WriteLine($\"long={hl.Run(la, lb, lc)} int={hi.Run(ia, ib, ic)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        std::string("long=123 int=123"));
}

// CONTROL / stdlib §4.4: a NON-generic class element.  This is the path #3512
// and #3529 fixed and #3530 must leave byte-identical — its thunk name gains no
// argument suffix and its call target is still `methodFns_[CompareTo]`.
BEZEL_TEST(array_sort_generic_class_elem_3530, control_non_generic_class_element_unchanged) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; "
        "class Box: IComparable<Box> "
        "  public Lead: long; "
        "  public V: int := 0; "
        "  public constructor(lead: long, v: int) Lead := lead; V := v; end constructor; "
        "  public method CompareTo(other: Box): int return V - other.V; end CompareTo; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Box(7, 3); "
        "    using var b := new Box(8, 1); "
        "    using var c := new Box(9, 2); "
        "    var arr := new Array<Box>(); "
        "    arr.Add(a); arr.Add(b); arr.Add(c); "
        "    arr.Sort(); "
        "    Console.WriteLine($\"{arr[0].V}{arr[1].V}{arr[2].V}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        std::string("123"));
}

// CONTROL / stdlib §4.4: an `int` element still routes to the `bz$intcmp` arm.
// The fix must not pull a value element into the object-comparator path.
BEZEL_TEST(array_sort_generic_class_elem_3530, control_int_element_still_sorts) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); "
        "    arr.Add(3); arr.Add(1); arr.Add(2); "
        "    arr.Sort(); "
        "    Console.WriteLine($\"{arr[0]}{arr[1]}{arr[2]}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        std::string("123"));
}

// CONTROL / spec §12: a VIRTUAL `CompareTo` on the same generic class.  That
// arm loads the slot from the OBJECT's vtable, and the per-instantiation
// `vt$<C>$specN` already carries the specialized method — measured correct on
// e57a1eb, i.e. this row passes on BOTH sides of the fix by design.  It is here
// so a later change that routes the virtual arm through the template (or the
// direct arm through the vtable) fails loudly instead of silently.
BEZEL_TEST(array_sort_generic_class_elem_3530, control_virtual_compareto_generic_class) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; "
        "class Box<T>: IComparable<Box<T>> "
        "  public Lead: T; "
        "  public V: int := 0; "
        "  public constructor(lead: T, v: int) Lead := lead; V := v; end constructor; "
        "  public virtual method CompareTo(other: Box<T>): int return V - other.V; end CompareTo; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Box<long>(7, 3); "
        "    using var b := new Box<long>(8, 1); "
        "    using var c := new Box<long>(9, 2); "
        "    var arr := new Array<Box<long>>(); "
        "    arr.Add(a); arr.Add(b); arr.Add(c); "
        "    arr.Sort(); "
        "    Console.WriteLine($\"{arr[0].V}{arr[1].V}{arr[2].V}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        std::string("123"));
}
