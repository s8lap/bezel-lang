// Spec-derived ACCEPT-CONTROL for issue #2926.
//
// §5.1 (Implicit conversions), §23.1 (Auto-properties), §9.2 (Record bodies),
// §22.13 (Static initialization order), §15.4 (Collection literals), §17.7
// (Typeless `new`), §8.6 (Boxing), §12 (Subtyping), v0.240.
//
// #2926 gave a field / auto-property INITIALIZER its first assignability check
// of any kind. The REJECT half is pinned in
// tests/lit/field_initializer_assignability_2926.bzl. THIS file pins the other
// half — the false-reject cost — because a check that has never run before is
// exactly the shape that breaks working code, and "it compiles" is not enough:
// every initializer's VALUE is read back.
//
// The shapes hit every arm `checkAssignable` can take from a field
// initializer: §5.1 constant narrowing at each sub-`int` width and its nullable
// composition; ordinary widening; `null` at a nullable value, a nullable
// reference and a nullable string; a record; an enum member; §8.6 boxing into
// `Object`; a §12 interface target satisfied by a subclass; §17.7's typeless
// `new`; a §15.4 collection literal (which has NO standalone type, so
// `checkAssignable` must no-op rather than reject); the static and
// auto-property spellings; and a record body. Passes both pre- and post-fix BY
// CONSTRUCTION — its job is to prove the new rejection did not widen past the
// population it is about.
//
// Each case counts its satisfied assertions and expects the full count, so no
// row is unobservable.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// A1 — every integer width §5.1's constant-narrowing rule covers, plain and
// nullable, plus an ordinary widening. 9 rows.
// ===========================================================================
BEZEL_TEST(sema_field_initializer_assignability_accept_2926, integer_widths_5_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module A2926a; "
        "public class Box "
        "  public _sb: sbyte  := -100; "
        "  public _b:  byte   := 250; "
        "  public _s:  short  := 30000; "
        "  public _us: ushort := 60000; "
        "  public _ui: uint   := 3; "
        "  public _ul: ulong  := 3; "
        "  public _l:  long   := 3; "
        "  public _bq: byte?  := 200; "
        "  public _sq: short? := 30000; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    if int(b._sb) = -100     then acc := acc + 1; end; "
        "    if int(b._b)  = 250      then acc := acc + 1; end; "
        "    if int(b._s)  = 30000    then acc := acc + 1; end; "
        "    if int(b._us) = 60000    then acc := acc + 1; end; "
        "    if int(b._ui) = 3        then acc := acc + 1; end; "
        "    if int(b._ul) = 3        then acc := acc + 1; end; "
        "    if b._l       = 3        then acc := acc + 1; end; "
        "    if (b._bq ?? 0) = 200   then acc := acc + 1; end; "
        "    if (b._sq ?? -1) = 30000 then acc := acc + 1; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 9);
}

// ===========================================================================
// A2 — the NON-numeric initializer shapes. Each reaches an arm of
// `checkAssignable` that had never been asked about a field before. 14 rows.
// ===========================================================================
BEZEL_TEST(sema_field_initializer_assignability_accept_2926, non_numeric_shapes_5_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module A2926b; "
        "public enum Color "
        "  Red, "
        "  Green; "
        "end Color; "
        "public record Pt(x: int, y: int); "
        "public interface IThing "
        "  method Go(): int; "
        "end IThing; "
        "public class Impl: IThing "
        "  public method Go(): int "
        "    return 7; "
        "  end Go; "
        "end Impl; "
        "public class Box "
        "  public _str:  string  := \"hi\"; "
        "  public _strq: string? := null; "
        "  public _iq:   int?    := null; "
        "  public _objq: Object? := null; "
        "  public _obj:  Object  := 5; "
        "  public _bool: bool    := true; "
        "  public _ch:   char    := 'x'; "
        "  public _dec:  decimal := 2.5m; "
        "  public _f:    float   := 1.5f; "
        "  public _d:    double  := 3; "
        "  public _c:    Color   := Color.Green; "
        "  public _pt:   Pt      := new Pt(1, 2); "
        "  public owned _th:  IThing        := new Impl(); "
        "  public owned _sb2: StringBuilder := new(); "
        "  public owned _arr: Array<int>    := [1, 2, 3]; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    if b._str.Length = 2     then acc := acc + 1; end; "
        "    if b._strq is null       then acc := acc + 1; end; "
        "    if b._iq   is null       then acc := acc + 1; end; "
        "    if b._objq is null       then acc := acc + 1; end; "
        "    if b._obj  is not null   then acc := acc + 1; end; "
        "    if b._bool               then acc := acc + 1; end; "
        "    if int(b._ch)  = 120     then acc := acc + 1; end; "
        "    if int(b._dec) = 2       then acc := acc + 1; end; "
        "    if int(b._f)   = 1       then acc := acc + 1; end; "
        "    if int(b._d)   = 3       then acc := acc + 1; end; "
        "    if int(b._c)   = 1       then acc := acc + 1; end; "
        "    if b._pt.x + b._pt.y = 3 then acc := acc + 1; end; "
        "    if b._th.Go()  = 7       then acc := acc + 1; end; "
        "    if b._arr.Count = 3     then acc := acc + 1; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 14);
}

// ===========================================================================
// A3 — the STATIC and AUTO-PROPERTY spellings, which take different codegen
// paths (a baked global constant for `static readonly`, the compiler-generated
// backing field for an auto-property) but share the one sema arm this fix
// touched. 6 rows.
// ===========================================================================
BEZEL_TEST(sema_field_initializer_assignability_accept_2926, static_and_property_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module A2926c; "
        "public class Box "
        "  public property P1: byte    := 200; "
        "  public property P2: int     := 4; "
        "  public property P3: string? := null; "
        "  public property P4: short?  := 30000; "
        "  public static readonly SR: byte  := 250; "
        "  public static readonly SQ: byte? := 200; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    if int(b.P1) = 200      then acc := acc + 1; end; "
        "    if b.P2      = 4        then acc := acc + 1; end; "
        "    if b.P3 is null         then acc := acc + 1; end; "
        "    if (b.P4 ?? -1) = 30000 then acc := acc + 1; end; "
        "    if int(Box.SR)  = 250   then acc := acc + 1; end; "
        "    if (Box.SQ ?? 0) = 200 then acc := acc + 1; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 6);
}

// ===========================================================================
// A5 — the GENERIC field, and the one case the first cut of this fix ACTUALLY
// BROKE (caught by the whole-codegen-area sweep, 1 failure in 11206:
// generic_field_direct_call_2068.class_box_function_field_42). A field whose
// declared type is a bare type parameter `U` is checked at the INSTANTIATION,
// not at the declaration — `Box<(int) -> int>` makes `V: U := Box.Noop`
// well-typed, but judging the unsubstituted `U` here reached
// `checkAssignable`'s Function→non-Function arm and rejected it:
//
//   M.bzl:3:18: the initializer for field `V` has a type that is not
//               implicitly convertible to the target type (§5.1)
//
// The gate is the PROPERTY "the declared type mentions an unsubstituted type
// parameter", answered by the same SUPERSET walk §22.3's ref/out check uses
// (#1756's lesson: a narrower guard is what made that check false-reject and
// get reverted four times) — not a list of the shapes that happened to break.
// The second field is the BARE-`U`-behind-a-generic-argument spelling, which a
// guard that only looked at the type's own head would miss.
// ===========================================================================
BEZEL_TEST(sema_field_initializer_assignability_accept_2926, generic_type_param_field_14_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module A2926e; "
        "class Box<U> "
        "  public V: U := Box<U>.Noop; "
        "  public owned A: Array<U> := new Array<U>(); "
        "  public static method Noop(x: int): int return x; end Noop; "
        "  public static method Id(x: int): int return x; end Id; "
        "  public static method Main(): int "
        "    using var b := new Box<(int) -> int>(); "
        "    b.V := Id; "
        "    return b.V(42) + b.A.Count + 7; "
        "  end Main; "
        "end Box;"), 49);
}

// ===========================================================================
// A4 — a RECORD BODY's field initializers (§9.2), the arm #2903's own T3
// exercises for the store width. The record spelling reaches the same
// `visitMember` FieldDecl arm, so the new check must admit exactly what the
// class spelling admits. 5 rows.
// ===========================================================================
BEZEL_TEST(sema_field_initializer_assignability_accept_2926, record_body_9_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module A2926d; "
        "public record Rec "
        "  public wl: long    := -42; "
        "  public wq: long?   := -42; "
        "  public wd: double? := 3; "
        "  public nn: sbyte   := -1; "
        "  public nb: byte    := 250; "
        "end Rec; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var r := new Rec(); "
        "    if r.wl = -42          then acc := acc + 1; end; "
        "    if (r.wq ?? 0L) = -42  then acc := acc + 1; end; "
        "    if (r.wd ?? 0.0) = 3.0 then acc := acc + 1; end; "
        "    if int(r.nn) = -1      then acc := acc + 1; end; "
        "    if int(r.nb) = 250     then acc := acc + 1; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 5);
}
