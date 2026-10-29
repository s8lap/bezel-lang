// Spec-derived regression tests for issue #2014 — moving an owned field out of a
// receiver whose type is the ENCLOSING method's own type parameter read GARBAGE
// and SEGV'd (`Box<U>` inside `Run<U>`, `var t := move box.f!`).
//
// §14 (generics / monomorphization) + §28.8/§28.8.1 (the move expression, v0.197):
// `move recv.f` where `f` is `owned` produces an Owned value; the field GEP that
// reads it must be computed on the receiver's SPECIALIZED-layout struct. Inside a
// monomorphized `Run<U>` (specialized U=long / U=Payload) the receiver `box:
// Box<U>` must resolve `U` to that concrete type so `Box`'s field `first: T` is
// laid out at its true width and the owned field `f` sits at the correct offset.
//
// The bug (#2014): fieldAddr's "unresolved generic arg" fallback (issue #828)
// fired for the bare-param arg `U` EVEN in the specialized body where the active
// substitution already binds `U` to its concrete type. It then overrode the
// correctly-computed layout substitution with `localGenericSubst_[box]` — a map
// keyed on the ENCLOSING method's param name `U`, NOT `Box`'s own param `T`. So
// classStruct(Box) could not find `T`, fell back to the `T→i32` TEMPLATE layout
// (`Box$spec1 = {ptr,i8,i32,ptr}`), and GEP'd owned field `f` at the wrong offset
// (16 instead of 24 for a `Box<long>` whose real object is `Box$spec0 =
// {ptr,i8,i64,ptr}`). The move-read then loaded 8 bytes short — into the `first`
// value — as the owned-field pointer: a garbage pointer → SEGV in the sink
// (confirmed on base 9d2f00a: AddressSanitizer SEGV in bz$C$Sink, reading a bad
// address derived from the `first` value). The concrete-generic case (`Box<long>`
// with an EXPLICIT type arg) works — the fault is specific to the receiver's type
// argument BEING the enclosing method's type parameter.
//
// The fix (codegen only, LlvmBackend_Layout.cpp fieldAddr): treat a bare-param arg
// as "unresolved" (needing the localGenericSubst_ fallback) only when the ACTIVE
// substitution does not already bind it — i.e. resolveTypeParamRef(arg) still
// yields a bare type parameter. In a specialized body `U` resolves to its concrete
// type, so lowerTypeRef already produced the correct `T→concrete` layout subst and
// the override is skipped. The #828 unspecialized case (no active binding) still
// takes the localGenericSubst_ fallback.
//
// A destructor-counter Cell (ctor ++, dtor --) is the exact-once oracle: Main
// returns Cell.live, so exitCode 0 == disposed exactly once (a leak returns >0; a
// double-dispose aborts under the harness's generated-code AddressSanitizer). The
// SEGV cases below ABORT under ASan on base; each runs clean and returns 0 with the
// fix.

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + generated-code ASan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A destructor-counter Cell: ctor ++, dtor --. A correct program leaves live==0;
// a double-dispose drives it negative or aborts under ASan (heap-use-after-free).
const char* kCell =
    "module M; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

// A reference-type payload used as the enclosing method's type argument `U` so the
// U=class instantiation is exercised alongside U=primitive.
const char* kPayload =
    "class Payload "
    "  public v: int := 7; "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Payload; ";

}  // namespace

// ===========================================================================
// #2014 — the move-read of an owned field on a receiver typed on the ENCLOSING
// method's own type parameter GEPs the correct specialized layout.
// ===========================================================================

// §14 / §28.8.1: value field (`first: T`, `T→i64`) laid out BEFORE the owned field
// `f`. On base the move-read GEPs `f` under the `T→i32` template layout (offset 16)
// while the object is the `T→i64` layout (offset 24) — reads part of the `first`
// value as a pointer → SEGV. With the fix `U` resolves to `long`, `f` is read at
// the correct offset, and the Cell is disposed exactly once. U = PRIMITIVE.
BEZEL_TEST(move_field_enclosing_type_param_receiver_2014,
           value_field_before_owned_primitive_arg_disposes_once_spec_14) {
    std::string src =
        std::string(kCell) +
        "class Box<T> "
        "  public first: T; "
        "  public owned f: Cell? := null; "
        "  public constructor(v: T) this.first := v; this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Run<U>(u: U) "
        "    using var box := new Box<U>(u); "
        "    var t: Cell? := move box.f!; "
        "    box.f := null; "
        "    C.Sink(move t!); "
        "  end Run; "
        "  public static method Main(): int "
        "    C.Run<long>(1234567890123L); "
        "    return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // Cell.live == 0: disposed exactly once
}

// §14 / §28.8.1: same shape but U = a CLASS type (reference). The `first: T` field
// lowers to a pointer either way, but the enclosing-type-param substitution path is
// the same; pins that the fix is not primitive-specific. Disposed exactly once.
//
// The call site is `C.Run<Payload>(p!)` — `Run<U>(u: U)` declares `u` PLAIN, not
// `owned`, so §22.3 (v0.314, #5388) admits no `move` call-site marker there and
// rejects the marked form ("the `move` call-site marker is only valid on an argument
// to an `owned` parameter"). The marker was dropped; the row's subject — the
// specialized-layout field GEP through an enclosing-type-param receiver — is
// untouched by that spelling.
BEZEL_TEST(move_field_enclosing_type_param_receiver_2014,
           value_field_before_owned_class_arg_disposes_once_spec_14) {
    std::string src =
        std::string(kCell) + kPayload +
        "class Box<T> "
        "  public first: T; "
        "  public owned f: Cell? := null; "
        "  public constructor(v: T) this.first := v; this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Run<U>(u: U) "
        "    using var box := new Box<U>(u); "
        "    var t: Cell? := move box.f!; "
        "    C.Sink(move t!); "
        "  end Run; "
        "  public static method Main(): int "
        "    var p: Payload? := new Payload(); "
        "    C.Run<Payload>(p!); "
        "    return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §14 / §28.8.1: the owned field is laid out at OFFSET 0 (first field after the
// header), value field after. The wrong-layout offset would still corrupt the read;
// the fix reads the field at its true offset regardless of field order. U=primitive.
BEZEL_TEST(move_field_enclosing_type_param_receiver_2014,
           owned_field_before_value_field_disposes_once_spec_14) {
    std::string src =
        std::string(kCell) +
        "class Box<T> "
        "  public owned f: Cell? := null; "
        "  public first: T; "
        "  public constructor(v: T) this.f := new Cell(); this.first := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Run<U>(u: U) "
        "    using var box := new Box<U>(u); "
        "    var t: Cell? := move box.f!; "
        "    C.Sink(move t!); "
        "  end Run; "
        "  public static method Main(): int "
        "    C.Run<long>(1234567890123L); "
        "    return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §14 / §28.8.1: TWO owned fields with a value field between them. Move only `f`;
// the owner's scope-end disposal must still dispose `g` (and NOT re-dispose the
// moved-out `f`). All owned-field GEPs must use the specialized layout. U=primitive.
// Two Cells created, each disposed exactly once → live == 0.
BEZEL_TEST(move_field_enclosing_type_param_receiver_2014,
           two_owned_fields_move_one_disposes_each_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class Box<T> "
        "  public first: T; "
        "  public owned f: Cell? := null; "
        "  public owned g: Cell? := null; "
        "  public constructor(v: T) this.first := v; this.f := new Cell(); this.g := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Run<U>(u: U) "
        "    using var box := new Box<U>(u); "
        "    var t: Cell? := move box.f!; "
        "    C.Sink(move t!); "
        "  end Run; "
        "  public static method Main(): int "
        "    C.Run<long>(1234567890123L); "
        "    return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// Not-a-regression guards.
// ---------------------------------------------------------------------------

// §14 / §28.8: the NO-MOVE control — a plain read of the owned field through the
// enclosing-type-param receiver (a null comparison, no move). This did not crash on
// base (the value is not dereferenced), but the fix makes the field GEP correct here
// too. Returns 0 (the field is non-null), proving the read observes the real slot.
BEZEL_TEST(move_field_enclosing_type_param_receiver_2014,
           no_move_read_of_owned_field_reads_correct_slot_spec_14) {
    std::string src =
        std::string(kCell) +
        "class Box<T> "
        "  public first: T; "
        "  public owned f: Cell? := null; "
        "  public constructor(v: T) this.first := v; this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Run<U>(u: U): int "
        "    using var box := new Box<U>(u); "
        "    if box.f = null then return 99; end if; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run<long>(1234567890123L); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // field non-null: read the correct slot
}

// §14 / §28.8.1: the CONCRETE-generic case (an EXPLICIT `Box<long>` type arg, NOT
// the enclosing type parameter) already worked and must stay working — the receiver
// resolves directly to the specialized layout with no enclosing-param indirection.
// Guards that the #2014 fix does not disturb the concrete path. Disposed once.
BEZEL_TEST(move_field_enclosing_type_param_receiver_2014,
           concrete_generic_receiver_still_disposes_once_spec_14) {
    std::string src =
        std::string(kCell) +
        "class Box<T> "
        "  public first: T; "
        "  public owned f: Cell? := null; "
        "  public constructor(v: T) this.first := v; this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Use() "
        "    using var box := new Box<long>(1234567890123L); "
        "    var t: Cell? := move box.f!; "
        "    box.f := null; "
        "    C.Sink(move t!); "
        "  end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
