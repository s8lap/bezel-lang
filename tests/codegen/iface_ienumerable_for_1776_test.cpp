// Codegen — #1776 / §21.4 / §29.2: `for var x in xs` where `xs` has STATIC
// type a user INTERFACE that extends `IEnumerable<T>` (rather than the concrete
// implementing class).
//
// Root cause (pre-fix): the for-in lowering (lowerIEnumerableFor) resolved
// `GetEnumerator` via findMethodByName on the source's static type. For a user
// interface `interface IMyColl: IEnumerable<int> end` that declares no
// GetEnumerator of its own — it inherits it from the *builtin* IEnumerable,
// which is not a synthesized itable-participating interface and so carries no
// dispatch slot — the lookup returned null and the statement fell through to
// the codegen NYI ("not yet implemented: codegen for this for statement",
// LlvmBackend_Stmts.cpp). Iterating the CONCRETE class compiled; the
// interface-typed value did not. sema §21.4 correctly admits the interface as
// iterable (#1775); the gap was purely codegen.
//
// Fix (§29.2): when every class implementing the interface resolves
// GetEnumerator to the SAME method (single implementer, or a base + subclasses
// inheriting its non-virtual GetEnumerator), the runtime object is necessarily
// an instance of that class (or such a subclass), so the for-in devirtualizes
// to a direct static call on the interface-typed receiver and reuses the
// concrete-class iteration path. A truly-polymorphic interface (two implementers
// with distinct GetEnumerators) still falls to the NYI — sound, never a
// miscompile.
//
// runAot returns the FULL int from Main() for values ≤ 255.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A concrete IEnumerator<int> plus a class implementing a user interface that
// extends IEnumerable<int>. Shared across the cases below.
const char* kBase =
    "module M; "
    "class IntRangeEnum: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
    "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end IntRangeEnum; "
    "interface IMyColl: IEnumerable<int> end IMyColl; "
    "class IntRange: IMyColl "
    "  protected lo: int; protected hi: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return new IntRangeEnum(lo, hi); end GetEnumerator; "
    "end IntRange; ";
}  // namespace

// #1776: the issue's exact repro — a for-in over an INTERFACE-typed
// IEnumerable<int> source sums the elements (was: codegen NYI).
BEZEL_TEST(iface_ienumerable_for_1776, interface_typed_source_sums) {
    BEZEL_EXPECT_EQ(run(
        std::string(kBase) +
        "class C "
        "  public static method Sum(xs: IMyColl): int "
        "    var t := 0; "
        "    for var x in xs do t := t + x; end; "
        "    return t; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var r := new IntRange(1, 5); "
        "    return C.Sum(r); "  // 1+2+3+4+5 = 15
        "  end Main; end C;"), 15);
}

// #1776: early `break` out of the interface-typed for-in disposes the
// enumerator and stops iterating at the break point.
BEZEL_TEST(iface_ienumerable_for_1776, interface_typed_source_break) {
    BEZEL_EXPECT_EQ(run(
        std::string(kBase) +
        "class C "
        "  public static method Sum(xs: IMyColl): int "
        "    var t := 0; "
        "    for var x in xs do t := t + x; if x >= 4 then break; end; end; "
        "    return t; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var r := new IntRange(1, 100); "
        "    return C.Sum(r); "  // 1+2+3+4 = 10
        "  end Main; end C;"), 10);
}

// #1776: an instance of a SUBCLASS of the sole implementer, iterated through the
// interface, devirtualizes to the shared (inherited, non-virtual)
// GetEnumerator and iterates correctly.
BEZEL_TEST(iface_ienumerable_for_1776, subclass_instance_via_interface) {
    BEZEL_EXPECT_EQ(run(
        std::string(kBase) +
        "class SubRange: IntRange "
        "  public constructor(lo: int, hi: int) base(lo, hi); end constructor; "
        "end SubRange; "
        "class C "
        "  public static method Sum(xs: IMyColl): int "
        "    var t := 0; "
        "    for var x in xs do t := t + x; end; "
        "    return t; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var r := new SubRange(1, 4); "
        "    return C.Sum(r); "  // 1+2+3+4 = 10
        "  end Main; end C;"), 10);
}

// #1776: an empty interface-typed source iterates zero times (MoveNext false on
// the first call).
BEZEL_TEST(iface_ienumerable_for_1776, empty_interface_typed_source) {
    BEZEL_EXPECT_EQ(run(
        std::string(kBase) +
        "class C "
        "  public static method Sum(xs: IMyColl): int "
        "    var t := 0; "
        "    for var x in xs do t := t + x; end; "
        "    return t + 7; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var r := new IntRange(5, 1); "  // lo > hi → empty
        "    return C.Sum(r); "  // 0 + 7 = 7
        "  end Main; end C;"), 7);
}

// #1776 regression guard: iterating the CONCRETE class directly (the unchanged
// pre-existing path) still works — the fix must not perturb it.
BEZEL_TEST(iface_ienumerable_for_1776, concrete_class_path_unchanged) {
    BEZEL_EXPECT_EQ(run(
        std::string(kBase) +
        "class C "
        "  public static method Sum(xs: IntRange): int "
        "    var t := 0; "
        "    for var x in xs do t := t + x; end; "
        "    return t; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var r := new IntRange(1, 5); "
        "    return C.Sum(r); "  // 15
        "  end Main; end C;"), 15);
}
