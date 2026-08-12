// Spec-derived regression test for issue #2901.
//
// §8.6 (Boxing) / §14.1 (generics are monomorphized) / §11.2 (interface
// declaration) / §8.5 (`ToString`), v0.240.
//
// §8.6 states, verbatim:
//
//     "When a value type is used where `Object` (or an interface implemented by
//      the value type) is expected, the value is **boxed**: a heap-allocated
//      wrapper holds the value. Boxed values are runtime-managed ([§28.9])."
//
// A RECORD passed to a generic parameter whose type argument is an INTERFACE it
// implements — `Nm<IArea>(rb)` — is exactly that sentence's case. It was never
// boxed.
//
// WHAT THE ISSUE SAID, AND WHAT IS ACTUALLY TRUE. The issue attributed the
// fault to "the object-shaped dereference the `lowerObjectToString` vtable path
// performs on what is actually an unboxed record value". The emitted IR refutes
// that — `lowerObjectToString` never runs. The monomorphization is:
//
//     define internal i32 @"C$Nm$spec$IArea"(%record.RBox %0) {
//     entry:
//       %1 = alloca ptr
//       ...
//       store ptr null, ptr %1              ; `var s: string := v.ToString()`
//       %4 = load ptr, ptr %1               ;   degraded to NULL (#2477)
//       %5 = getelementptr %bz.string, ptr %4, i32 0, i32 1
//       %str.len = load i64, ptr %5         ; SEGV: read at null + 8
//
// i.e. the parameter arrives as `%record.RBox` BY VALUE, `v.ToString()` fails to
// lower at all, #2477's zero-init fallback stores null into `s`, and the fault
// is the unguarded `s.Length` on that null. The two repros the issue filed as
// separate symptoms are ONE defect: the `where T: IArea` spelling degrades its
// RETURN to 0 through #2491 instead of faulting, printing 0 for an expected 6.
//
// ROOT CAUSE. The §8.6 box IS applied to every argument, by `boxForTarget`
// inside `buildOrderedArgs`. But the method-level substitution is installed
// later, by `applyMethodGenericSubst` — measured: at `buildOrderedArgs` time
// `genericSubstSemaTy_` is EMPTY, so the formal `v: T` is still the bare
// type-parameter ref (Named, `declaration == nullptr`) that
// `isObjectOrInterfaceTarget` cannot classify. It answered "not an
// Object/interface target", so the box was skipped for EVERY generic parameter
// at EVERY call site. `emitGenericSpecialization` then builds the specialized
// signature from `args[i]->getType()` ("each param type defaults to its
// argument's LLVM type"), so the body was emitted with `T` bound to the
// interface while its parameter was a record by value.
//
// This is the exact mirror of #1279, which gave the SOURCE side of
// `boxForTarget` its substitution; the TARGET side never got one.
//
// WHAT IS NOT PART OF THIS DEFECT — both measured, both left alone:
//
//   * The `internal: codegen cannot dispatch member 'ToString' on receiver of
//     type 'T' ... (#2477)` line still appears. It is PRE-EXISTING noise from
//     the DEAD generic template body (`emittingGenericTemplateBody_`), not a
//     symptom of the record shape: the CLASS-implementor spelling — which
//     already worked before this fix and is pinned by
//     object_tostring_producer_residual_2867 — emits the byte-identical warning
//     while printing the correct value. It belongs to the #2477/#2491/#2924
//     degrade family.
//
//   * A 24-byte box leak per boxed argument. Also PRE-EXISTING and NOT generic:
//     the same record passed to a WRITTEN-OUT `IArea` parameter, with no
//     generics anywhere in the program, leaks exactly 24 bytes in 1 allocation
//     today. Filed separately (#2952) as an argument-position box-lifetime gap.
//     It is why the cells that now mint a box run under `runAotCheckedLenient`
//     (generated-code ASan WITHOUT LSan): the SEGV is what must not come back,
//     and the leak is expected and tracked. Promote them to `runAotChecked`
//     when #2952 lands.
//
// EVIDENCE DISCIPLINE. "exit 0" is not an oracle here — the `where T: IArea`
// spelling exited 0 while printing the wrong answer. Every cell asserts the
// VALUE, and each value is one only the correct lowering produces: 6 = 2*3 read
// back through the interface, 16 = the §8.5 rendering of `RBox(2, 3)` (the same
// number the non-generic `IArea` parameter produces, so the two spellings are
// pinned to agree), 116 = 16 + the receiver's own field. The two CONTROL cells
// pass both before and after by design, and are labelled as controls.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

std::string out(const bezel::test::JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// The shared §11.2 interface + §9 record implementor the interface-bound cells
// use. `RBox(2, 3)` so `Area()` is 6 — a value neither a zero-degrade nor a
// field mix-up produces.
const char* kIfaceAndRecord =
    "module App; "
    "interface IArea "
    "  method Area(): int; "
    "end IArea; "
    "record RBox(W: int, H: int): IArea "
    "  public method Area(): int "
    "    return W * H; "
    "  end Area; "
    "end RBox; ";

}  // namespace

// ---------------------------------------------------------------------------
// The two reported repros — one defect, two symptoms.
// ---------------------------------------------------------------------------

// #2901 / §8.6 + §14.1 — repro 1. `T` explicitly bound to an INTERFACE, called
// with a RECORD that implements it. PRE-FIX: `SEGV on unknown address 0x8` in
// `C$Nm$spec$IArea`. The expected 16 is the §8.5 rendering length of
// `RBox(2, 3)` — pinned against the non-generic spelling by the cell below.
BEZEL_TEST(codegen_generic_interface_bound_record_box_2901,
           record_arg_interface_bound_type_param_tostring_8_6) {
    auto r = runAotCheckedLenient(
        std::string(kIfaceAndRecord) +
        "class C "
        "  public static method Nm<T>(v: T): int "
        "    var s: string := v.ToString(); "
        "    return s.Length; "
        "  end Nm; "
        "  public static method Main(): int "
        "    var rb: RBox := new RBox(2, 3); "
        "    Console.WriteLine(Nm<IArea>(rb)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_EQ(out(r), std::string("16\n"));
}

// #2901 / §8.6 + §11.2 — repro 2, the SILENT WRONG VALUE, which is the worse
// symptom: it exits 0. An interface METHOD called through the bound `T`.
// PRE-FIX: printed `0`; §11.2's `Area()` on `RBox(2, 3)` is 6.
BEZEL_TEST(codegen_generic_interface_bound_record_box_2901,
           record_arg_interface_bound_type_param_iface_method_11_2) {
    auto r = runAotCheckedLenient(
        std::string(kIfaceAndRecord) +
        "class C "
        "  public static method Nm<T>(v: T): int where T: IArea "
        "    return v.Area(); "
        "  end Nm; "
        "  public static method Main(): int "
        "    var rb: RBox := new RBox(2, 3); "
        "    Console.WriteLine(Nm<IArea>(rb)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_EQ(out(r), std::string("6\n"));
}

// ---------------------------------------------------------------------------
// The axis the issue listed as "not investigated" — it was broken too.
// ---------------------------------------------------------------------------

// #2901 / §8.6 — an `Object`-bound `T`. §8.6's sentence names `Object` and "an
// interface implemented by the value type" in the SAME breath, so the fix must
// cover both; the issue explicitly left this unmeasured. It SEGV'd identically
// (`SUMMARY: AddressSanitizer: SEGV bezel in C$Nm$spec`), for a RECORD argument
// and for a PRIMITIVE one. 16 = the record's rendering, 2 = `"41".Length`.
BEZEL_TEST(codegen_generic_interface_bound_record_box_2901,
           record_and_primitive_arg_object_bound_type_param_8_6) {
    auto r = runAotCheckedLenient(
        "module App; "
        "record RBox(W: int, H: int) "
        "  public method Area(): int "
        "    return W * H; "
        "  end Area; "
        "end RBox; "
        "class C "
        "  public static method Nm<T>(v: T): int "
        "    var s: string := v.ToString(); "
        "    return s.Length; "
        "  end Nm; "
        "  public static method Main(): int "
        "    var rb: RBox := new RBox(2, 3); "
        "    Console.WriteLine(Nm<Object>(rb)); "
        "    Console.WriteLine(Nm<Object>(41)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_EQ(out(r), std::string("16\n2\n"));
}

// #2901 / §14.1 — an INSTANCE generic method, so `args[0]` is the receiver and
// the parameter sits at offset 1. Pins the new coercion loop's index alignment:
// an off-by-one would box the RECEIVER instead of the argument. 116 = 16 + the
// receiver's own field, so the receiver must have survived intact.
BEZEL_TEST(codegen_generic_interface_bound_record_box_2901,
           instance_generic_method_receiver_offset_14_1) {
    auto r = runAotCheckedLenient(
        std::string(kIfaceAndRecord) +
        "class Holder "
        "  private _k: int; "
        "  public constructor(k: int) "
        "    _k := k; "
        "  end constructor; "
        "  public method Nm<T>(v: T): int "
        "    var s: string := v.ToString(); "
        "    return s.Length + _k; "
        "  end Nm; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h: Holder := new Holder(100); "
        "  var rb: RBox := new RBox(2, 3); "
        "  Console.WriteLine(h.Nm<IArea>(rb)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_EQ(out(r), std::string("116\n"));
}

// ---------------------------------------------------------------------------
// CONTROLS — these pass before AND after, deliberately.
// ---------------------------------------------------------------------------

// #2901 / §8.6 — the REGRESSION GUARD for the over-boxing direction. `T` bound
// to a VALUE type (a record, and a primitive) must NOT box: §8.6's rule is
// keyed on the TARGET being `Object`/an interface, and neither is. Both a
// `T`-typed parameter and a `T`-typed RETURN are exercised, because an
// over-box would make the returned value a pointer and `q.Sum()` would read the
// box header as fields. This cell runs under the FULL checker — it mints no box
// at all, so it must stay leak-free, which is itself the evidence that nothing
// was boxed. 14 = `Pt(2, 3)`'s rendering, 2 = `"41".Length`, 5 = 2+3, 7 = 7.
BEZEL_TEST(codegen_generic_interface_bound_record_box_2901,
           value_bound_type_param_still_unboxed_8_6) {
    auto r = runAotChecked(
        "module App; "
        "record Pt(X: int, Y: int) "
        "  public method Sum(): int "
        "    return X + Y; "
        "  end Sum; "
        "end Pt; "
        "class C "
        "  public static method Val<T>(v: T): int "
        "    var s: string := v.ToString(); "
        "    return s.Length; "
        "  end Val; "
        "  public static method Twice<T>(v: T): T "
        "    return v; "
        "  end Twice; "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(2, 3); "
        "    Console.WriteLine(Val<Pt>(p)); "
        "    Console.WriteLine(Val<int>(41)); "
        "    var q: Pt := Twice<Pt>(p); "
        "    Console.WriteLine(q.Sum()); "
        "    Console.WriteLine(Twice<int>(7)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_EQ(out(r), std::string("14\n2\n5\n7\n"));
}

// #2901 / §8.6 — the CLASS-implementor spelling of the reported shape, which
// already worked (a class argument is already a pointer, so passing it
// unboxed happened to match what an interface-bound `T` expects). It must keep
// working and keep NOT boxing — a reference source is not §8.6's case. Kept
// here as the control that localizes the defect to VALUE-typed arguments, and
// as the cell whose identical `internal:` #2477 warning shows that warning is
// pre-existing template-body noise rather than a symptom. 8 =
// `"App.CBox".Length` (§8.5's default for classes: the type's qualified name,
// which since #2934/v0.252 is stdlib §2.13's module-path-qualified `FullName`
// rendering — it read 4 = `"CBox".Length` before that).
BEZEL_TEST(codegen_generic_interface_bound_record_box_2901,
           class_implementor_spelling_unchanged_control_8_5) {
    auto r = runAotChecked(
        "module App; "
        "interface IArea2 "
        "  method Area(): int; "
        "end IArea2; "
        "class CBox: IArea2 "
        "  private _w: int; "
        "  private _h: int; "
        "  public constructor(w: int, h: int) "
        "    _w := w; "
        "    _h := h; "
        "  end constructor; "
        "  public method Area(): int "
        "    return _w * _h; "
        "  end Area; "
        "end CBox; "
        "class C "
        "  public static method Nm<T>(v: T): int "
        "    var s: string := v.ToString(); "
        "    return s.Length; "
        "  end Nm; "
        "  public static method Main(): int "
        "    using var cb: CBox := new CBox(2, 3); "
        "    Console.WriteLine(Nm<IArea2>(cb)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    // #2934 (§8.5, v0.252): the §8.5 class default is module-path-
    // qualified, so the rendered name is "App.CBox" (8) — was "CBox" (4).
    BEZEL_EXPECT_EQ(out(r), std::string("8\n"));
}
