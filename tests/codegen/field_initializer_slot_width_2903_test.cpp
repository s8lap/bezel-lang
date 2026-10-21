// Spec-derived regression test for issue #2903.
//
// §5.1 (Implicit conversions — "Constant narrowing" and "Nullable
// conversions") / §7.2 (Nullable value types) / §23.1 (Auto-properties) /
// §9.2 (Record bodies) / §22.13 (Static initialization order), v0.240.
//
// THE DEFECT. A field / auto-property INITIALIZER was stored into its slot at
// the INITIALIZER's lowered width, not the SLOT's. `store i32` into a 1-, 2- or
// 3-byte-wide slot is perfectly VALID IR under opaque pointers, so nothing —
// not the LLVM verifier, not any existing test — ever saw it. Measured on
// c7404bf:
//
//   public _q:  byte?  := 200;    reads back WRONG (#2903's own repro: the
//   public _s:  short? := 30000;  program returned 16 where 21 is correct)
//   public _a:  sbyte  := -1;     reads back RIGHT, but writes `FF FF FF` over
//                                 the three following `byte` fields (255, 255,
//                                 255 where 0, 0, 0 is correct)
//   record { wl: long := -42; }   reads 4294967254 — the record body loop had
//   record { wq: long? := -42; }  NEITHER direction of §5.1's conversion, so
//   record { wd: double? := 3; }  these read 4294967254, null and null
//   static readonly SQ: byte? := 200;   ICE — "Global variable initializer type
//                                 does not match global variable type!"
//   static _mq: byte? := 200;     reads null; `static _ml: long := -42;` reads
//                                 4294967254
//
// THE CAUSE. `coerceNumericToType` is §5.1's WIDENING direction only, by
// construction (`srcBits < targetBits`), and each of the four field-initializer
// store sites had its own hand-written idea of what to do around it: the class
// instance site called it and stored whatever came back; the record body site
// called nothing at all; the baked-static site built the constant AT the slot
// type (`llvm::ConstantInt::get` on a `{i8, T}` struct — an unchecked
// `cast<IntegerType>`); the runtime-static site stored the bare value. §5.1
// admits the other direction — verbatim: "**Constant narrowing.** An expression
// of integer type that is a **compile-time constant** ([§17.14]) implicitly
// converts to any integer type whose range contains its value: `var b: byte :=
// 50;` …" — and under **Nullable conversions**: "The two rules compose through
// at most one `?` on each side ([§7.10]: `?` is idempotent): `S` converts to
// `T?` via `S → T → T?`". Nothing narrowed the initializer: sema retypes such a
// constant for a LOCAL declaration and for an assignment statement, but a field
// initializer is fed to neither, so it stays `int` and lowers to i32.
//
// THE FIX gives that rule ONE definition — `Emitter::coerceInitializerToSlot`,
// which matches §7.2's tagged `{i8 hasValue; T}` slot at its PAYLOAD width and
// everything else at the slot's own width, in BOTH of §5.1's directions — and
// applies it at all four initializer store sites (`emitInstanceInit`, the record
// body loop in `lowerNew`, the runtime static store in the sfinit, and
// `constInitForExpr` for the baked static constant).
//
// NOT FIXED HERE, filed separately: sema never checks a field initializer for
// assignability at all, so `public _q: byte := 300;` is admitted where §5.1 says
// it is "a **compile error** at the conversion site — never a silent
// truncation". Post-fix it truncates to 44 consistently at every spelling
// instead of also corrupting the neighbouring field; the missing REJECT is a
// sema matter, not this one.
//
// Every case asserts a VALUE (or a canary field's value), never merely that the
// program compiles. Observables are kept under 256 — the exit status is
// WEXITSTATUS-masked to 8 bits.

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
// T1 — the issue's own repro, widened to every sub-i32 width. §5.1's constant
// narrowing composed with its nullable conversion (`S → T → T?`) makes each of
// these well-typed, so each must read back its own value.
// Pre-fix: 16 (only the `int?` row, whose payload width already matched).
// Post-fix: 31.
// ===========================================================================
BEZEL_TEST(codegen_field_initializer_slot_width_2903, nullable_sub_i32_field_5_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module F2903a; "
        "public class Box "
        "  public _q:  byte?   := 200; "
        "  public _sb: sbyte?  := -100; "
        "  public _s:  short?  := 30000; "
        "  public _us: ushort? := 60000; "
        "  public _i:  int?    := 200; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    if (b._q  ?? 0) = 200   then acc := acc + 1; end; "
        "    if (b._sb ?? -1) = -100  then acc := acc + 2; end; "
        "    if (b._s  ?? -1) = 30000 then acc := acc + 4; end; "
        "    if (b._us ?? 0) = 60000 then acc := acc + 8; end; "
        "    if (b._i  ?? -1) = 200   then acc := acc + 16; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 31);
}

// ===========================================================================
// T2 — the NON-nullable spelling of the same defect, which #2903 did not
// report because it reads back CORRECTLY: the over-wide `store i32` lands its
// low byte in the right place and its other three bytes in the NEXT fields.
// `sbyte := -1` is `FF FF FF FF`, so the three following `byte` fields — which
// carry no initializer and are therefore §7.9's default 0 — read 255 pre-fix.
// This is the case that proves the fix is about the STORE WIDTH, not about the
// nullable wrap.
// Pre-fix: 1 (only `_a` itself). Post-fix: 15.
// ===========================================================================
BEZEL_TEST(codegen_field_initializer_slot_width_2903, non_nullable_over_wide_clobber_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module F2903b; "
        "public class Box "
        "  public _a: sbyte := -1; "
        "  public _b: byte; "
        "  public _c: byte; "
        "  public _d: byte; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    if int(b._a) = -1 then acc := acc + 1; end; "
        "    if int(b._b) = 0  then acc := acc + 2; end; "
        "    if int(b._c) = 0  then acc := acc + 4; end; "
        "    if int(b._d) = 0  then acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 15);
}

// ===========================================================================
// T3 — a RECORD BODY's field / auto-property initializers (§9.2). This loop is
// a separate hand-written copy that had NEITHER of §5.1's directions, so it was
// broken widening AND narrowing: pre-fix `wl` read 4294967254 (the i32 bits
// zero-filled into the i64 slot), `wq` and `wd` read `null` (a bare i32 written
// over the tagged `{i8, T}` slot leaves the payload untouched), and `nn`'s
// 4-byte store put `FF FF FF` over the three canaries.
// Pre-fix: 8 (only `nn` itself). Post-fix: 127.
// ===========================================================================
BEZEL_TEST(codegen_field_initializer_slot_width_2903, record_body_initializer_9_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module F2903c; "
        "public record Rec "
        "  public wl: long    := -42; "
        "  public wq: long?   := -42; "
        "  public wd: double? := 3; "
        "  public nn: sbyte   := -1; "
        "  public p1: byte; "
        "  public p2: byte; "
        "  public p3: byte; "
        "end Rec; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var r := new Rec(); "
        "    if r.wl = -42          then acc := acc + 1; end; "
        "    if (r.wq ?? 0L) = -42  then acc := acc + 2; end; "
        "    if (r.wd ?? 0.0) = 3.0 then acc := acc + 4; end; "
        "    if int(r.nn) = -1      then acc := acc + 8; end; "
        "    if int(r.p1) = 0       then acc := acc + 16; end; "
        "    if int(r.p2) = 0       then acc := acc + 32; end; "
        "    if int(r.p3) = 0       then acc := acc + 64; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 127);
}

// ===========================================================================
// T4 — a `readonly` STATIC of nullable value type (§22.13). Its literal is
// BAKED into the backing global's LLVM initializer, and that global's type is
// §7.2's `{i8 hasValue; T}` struct — pre-fix the constant was built at the
// STRUCT type (`ConstantInt::get` on a struct is an unchecked
// `cast<IntegerType>`) and the compile ABORTED: "Global variable initializer
// type does not match global variable type!". A `null` initializer must still
// bake the all-zero (hasValue = 0) form, and a `char?` — whose literal is NOT
// one of the baked shapes, so it takes the textual-order runtime store instead
// — must reach the slot the same way: the two over-acceptance controls for a
// fix that could otherwise wrap everything PRESENT.
// Pre-fix: the compile aborted. Post-fix: 31.
// ===========================================================================
BEZEL_TEST(codegen_field_initializer_slot_width_2903, readonly_static_nullable_22_13) {
    BEZEL_EXPECT_EQ(runProg(
        "module F2903d; "
        "public class H "
        "  public static readonly SQ: byte?   := 200; "
        "  public static readonly SS: short?  := 30000; "
        "  public static readonly SN: byte    := 200; "
        "  public static readonly SZ: byte?   := null; "
        "  public static readonly SC: char?   := 'a'; "
        "end H; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    if (H.SQ ?? 0) = 200   then acc := acc + 1; end; "
        "    if (H.SS ?? -1) = 30000 then acc := acc + 2; end; "
        "    if int(H.SN) = 200      then acc := acc + 4; end; "
        "    if H.SZ = null          then acc := acc + 8; end; "
        "    if (H.SC ?? 'z') = 'a'  then acc := acc + 16; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 31);
}

// ===========================================================================
// T5 — the NON-baked static store: a mutable static initializes at runtime in
// textual order (§22.13), through a store site that had neither §5.1 direction
// nor §7.2's wrap. Pre-fix `_mq` read `null` and `_ml` read 4294967254.
// (The §22.12 rule-2 declaration-site warning on a mutable primitive static is
// expected and is not an error.)
// Pre-fix: 4. Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_field_initializer_slot_width_2903, mutable_static_runtime_store_22_13) {
    BEZEL_EXPECT_EQ(runProg(
        "module F2903e; "
        "public class H "
        "  public static _mq: byte? := 200; "
        "  public static _ml: long  := -42; "
        "  public static _mn: sbyte := -1; "
        "end H; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    if (H._mq ?? 0) = 200 then acc := acc + 1; end; "
        "    if H._ml = -42         then acc := acc + 2; end; "
        "    if int(H._mn) = -1     then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 7);
}

// ===========================================================================
// T6 — the AUTO-PROPERTY spelling of the initializer (§23.1: an auto-property's
// storage is a field slot, `property X: T := v`). Same store site, same defect.
// Pre-fix: 2 (only the non-nullable row, which reads back right by accident).
// Post-fix: 3.
// ===========================================================================
BEZEL_TEST(codegen_field_initializer_slot_width_2903, auto_property_initializer_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module F2903f; "
        "public class H "
        "  public property AQ: byte? := 200; "
        "  public property AN: byte  := 200; "
        "end H; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var h := new H(); "
        "    if (h.AQ ?? 0) = 200 then acc := acc + 1; end; "
        "    if int(h.AN) = 200    then acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 3);
}

// ===========================================================================
// T7 — OVER-ACCEPTANCE CONTROLS. The fix widens what reaches a field slot, so
// every shape that ALREADY worked must be bit-identical: the §5.1 WIDENING
// rows (`long? := 42`, `double? := 1.5`), a nullable REFERENCE field (whose
// slot is a bare `ptr`, NOT a tagged struct — it must not be wrapped), a
// `null`-initialized nullable value field (must stay `null`, not become a
// present zero), a nullable RECORD field (an AGGREGATE payload that
// `nullableValuePayloadTy` deliberately excludes and this fix must pass
// through untouched), a `bool?` (an i1 payload, which §5.1's "Between integers
// and `bool`" row excludes from every numeric conversion), and an `Object`
// field taking §8.6's boxing.
// Passes both with and without the fix, BY CONSTRUCTION — that is the point.
// Post-fix: 127.
// ===========================================================================
BEZEL_TEST(codegen_field_initializer_slot_width_2903, unchanged_shapes_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module F2903g; "
        "public record Pt "
        "  public x: int; "
        "  public y: int; "
        "end Pt; "
        "public class Box "
        "  public _l: long?    := 42; "
        "  public _d: double?  := 1.5; "
        "  public _r: string?  := \"hi\"; "
        "  public _z: int?     := null; "
        "  public _p: Pt?      := new Pt(x := 3, y := 4); "
        "  public _b: bool?    := true; "
        "  public _o: Object   := 7; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    if (b._l ?? 0L) = 42    then acc := acc + 1; end; "
        "    if (b._d ?? 0.0) = 1.5  then acc := acc + 2; end; "
        "    if (b._r ?? \"\") = \"hi\" then acc := acc + 4; end; "
        "    if b._z = null          then acc := acc + 8; end; "
        "    var p := b._p!; "
        "    if p.x = 3 and p.y = 4 then acc := acc + 16; end; "
        "    if (b._b ?? false) then acc := acc + 32; end; "
        "    if b._o is int then acc := acc + 64; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 127);
}
