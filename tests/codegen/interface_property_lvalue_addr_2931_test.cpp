// Spec-derived regression test for issue #2931.
//
// §11.2 (Interface declaration) / §23.1 (Auto-properties) / §22.3 (Parameter
// modifiers — `ref`/`out`) / §12.1 (Members are non-virtual by default) /
// §23.3 (Block-form properties), v0.240.
//
// THE DEFECT. `lvalueAddr` returned an ADDRESS for a member access whose
// receiver's static type is an INTERFACE and whose member is a property, and
// that address was a GEP into the RECEIVER at the interface property's ORDINAL
// among the interface's own declarations. Every consumer that reached the shape
// therefore read and wrote an unrelated field of the object — silently, with no
// diagnostic and (usually) no crash.
//
// MEASURED on 10bd6d3 (the parent commit):
//
//   `M.Bump(ref i.IP)` on `class Box: IHasQ` with fields `_z`, `_v` and `IP`
//   backed by `_v`  ->  `z=5, v=0`.  Correct is `z=0, v=5`.
//   The CONCRETE-type spelling `M.Bump(ref b.IP)` gave `z=0, v=5` — correct —
//   so only the interface-typed receiver mis-resolved.
//
//   `M.Fill(out i.IP)`                       ->  `z=9, v=0`.
//
//   The index is NOT a constant 0, as the filing inferred. An interface
//   declaring `A` then `B`, implemented by a class whose first two fields are
//   `_f0`, `_f1`, put `ref i.B`'s write into `_f1` and `ref i.A`'s into `_f0` —
//   the property's ORDINAL. So with FIVE interface properties and a ONE-field
//   implementer, `ref i.E` was an ASan heap-buffer-overflow: "WRITE of size 4
//   … 12 bytes after 16-byte region". This is memory corruption, not a wrong
//   value, and T4 below is the shape that says so.
//
// THE CAUSE. An interface property and a class auto-property are SPELLED
// IDENTICALLY (`property P: T;`) and the parser records both as
// `PropertyBodyKind::Auto` (Parser_Decls.cpp — the interface arm's "simple
// signature form"). `Emitter::isAutoProperty` tests exactly that bodyKind, so
// `storageMemberOf` — "the central GEP-address resolution function for a plain
// member-access read/write" — handed back the interface's own PropertySymbol,
// and `fieldAddr` GEP'd the interface's phantom struct layout at that member's
// index. §11.2, verbatim: "Interface members are implicitly public, have no
// body, and end with `;`. A class implements an interface implicitly: by
// declaring matching public members." An interface property has no storage of
// its own and the implementing class's matching member owns whatever storage
// exists — so the correct answer is "no address", which is exactly what
// `lvalueAddr` already produced for the concrete-type spelling (a class's
// implementing property carries accessor bodies, `PropertyBodyKind::Block`,
// and so was never classified as storage).
//
// THE FIX declines in `storageMemberOf`. That is a PROPERTY — "does this
// member have a storage slot of its own?" — not an enumeration of consumers:
// every `ref`/`out` call site already falls back to §22.3's copy-in/copy-out
// bracket (`emitAccessorRefArgCopyIn`) the moment `lvalueAddr` returns null,
// and that bracket drives the getter and setter through the dispatching
// helpers. So the whole family is routed at once.
//
// WHY THE TESTS BELOW ASSERT NEIGHBOURS. A wrong-slot write is invisible to a
// test whose target property happens to be backed by field 0, and invisible to
// any test that only checks "the program did not crash". Every case here
// declares its fields in a KNOWN order with the property's backing field NOT
// first, and asserts EVERY field — so the defect is caught by the neighbour
// being disturbed as well as by the target being wrong.
//
// The cross-module half (an IMPORTED interface property, whose classification
// rides the `MetaMember::isAutoProperty` wire bit rather than the local decl)
// lives in tests/module/iface_property_refarg_import_2931_test.cpp.

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
// T1 — #2931's own repro: a `ref` argument through an interface-typed receiver,
// with the CONCRETE-type spelling of the same call beside it as the control.
// `IP` is backed by `_v` (field index 1), `_z` is field index 0, so the
// pre-fix write into index 0 shows up as BOTH `v` being 0 and `z` being 5.
// Pre-fix: 8 (only the concrete control's two bits).
// Post-fix: 15.
// ===========================================================================
BEZEL_TEST(codegen_interface_property_lvalue_addr_2931, ref_arg_through_interface_22_3) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2931a; "
        "public interface IHasQ "
        "  property IP: int; "
        "end IHasQ; "
        "public class Box: IHasQ "
        "  public _z: int := 0; "
        "  public _v: int := 0; "
        "  public property IP: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Bump(ref n: int): int "
        "    n := n + 5; "
        "    return n; "
        "  end Bump; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    var i: IHasQ := b; "
        "    var r := M.Bump(ref i.IP); "
        "    if b._v = 5 then acc := acc + 1; end; "
        "    if b._z = 0 then acc := acc + 2; end; "
        "    if r = 5 then acc := acc + 4; end; "
        "    using var b2 := new Box(); "
        "    M.Bump(ref b2.IP); "
        "    if b2._v = 5 and b2._z = 0 then acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 15);
}

// ===========================================================================
// T2 — the address is the property's ORDINAL, not a constant 0. An interface
// declaring `A` then `B`; the implementing class puts two unrelated fields
// FIRST so neither property is backed by index 0 or 1. Pre-fix `ref i.A` wrote
// `_f0` and `ref i.B` wrote `_f1`; both `_a` and `_b` stayed 0.
// Pre-fix: 0.  Post-fix: 3.
// ===========================================================================
BEZEL_TEST(codegen_interface_property_lvalue_addr_2931, ordinal_not_zero_11_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2931b; "
        "public interface ITwo "
        "  property A: int; "
        "  property B: int; "
        "end ITwo; "
        "public class Box: ITwo "
        "  public _f0: int := 0; "
        "  public _f1: int := 0; "
        "  public _a: int := 0; "
        "  public _b: int := 0; "
        "  public property A: int get => _a; set => do _a := value; end; end; "
        "  public property B: int get => _b; set => do _b := value; end; end; "
        "end Box; "
        "public class M "
        "  public static method Bump(ref n: int): int "
        "    n := n + 5; "
        "    return n; "
        "  end Bump; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b1 := new Box(); "
        "    var i1: ITwo := b1; "
        "    M.Bump(ref i1.B); "
        "    if b1._b = 5 and b1._a = 0 and b1._f0 = 0 and b1._f1 = 0 then "
        "      acc := acc + 1; end; "
        "    using var b2 := new Box(); "
        "    var i2: ITwo := b2; "
        "    M.Bump(ref i2.A); "
        "    if b2._a = 5 and b2._b = 0 and b2._f0 = 0 and b2._f1 = 0 then "
        "      acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 3);
}

// ===========================================================================
// T3 — an `out` argument, the second live `lvalueAddr` consumer the filing
// listed as UNPROBED. §22.3, verbatim, on the accessor form: "for `ref`, the
// getter is called once to produce the callee's slot value (for `out` no
// getter call occurs); on **normal return** the setter is called once with the
// slot's final value."
// Pre-fix: 0 (`z=9, v=0`).  Post-fix: 3.
// ===========================================================================
BEZEL_TEST(codegen_interface_property_lvalue_addr_2931, out_arg_through_interface_22_3) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2931c; "
        "public interface IHasQ "
        "  property IP: int; "
        "end IHasQ; "
        "public class Box: IHasQ "
        "  public _z: int := 0; "
        "  public _v: int := 0; "
        "  public property IP: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Fill(out n: int): int "
        "    n := 9; "
        "    return n; "
        "  end Fill; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    var i: IHasQ := b; "
        "    M.Fill(out i.IP); "
        "    if b._v = 9 then acc := acc + 1; end; "
        "    if b._z = 0 then acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 3);
}

// ===========================================================================
// T4 — THE MEMORY-SAFETY SHAPE. Five simple-form interface properties, an
// implementer with exactly ONE field: the ordinal for `E` is 4, so the write
// landed 16 bytes into a 16-byte object. Measured on 10bd6d3 under the probe's
// generated-code ASan: "heap-buffer-overflow … 12 bytes after 16-byte region".
// The `--full` gate runs every test program under ASan, so this case fails
// there on the fault as well as failing here on the value.
// Pre-fix: 0 (and an ASan abort under `--full`).  Post-fix: 1.
// ===========================================================================
BEZEL_TEST(codegen_interface_property_lvalue_addr_2931, past_the_end_write_11_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2931d; "
        "public interface IFive "
        "  property A: int; "
        "  property B: int; "
        "  property C: int; "
        "  property D: int; "
        "  property E: int; "
        "end IFive; "
        "public class Tiny: IFive "
        "  public _v: int := 0; "
        "  public property A: int get => _v; set => do _v := value; end; end; "
        "  public property B: int get => _v; set => do _v := value; end; end; "
        "  public property C: int get => _v; set => do _v := value; end; end; "
        "  public property D: int get => _v; set => do _v := value; end; end; "
        "  public property E: int get => _v; set => do _v := value; end; end; "
        "end Tiny; "
        "public class M "
        "  public static method Bump(ref n: int): int "
        "    n := n + 5; "
        "    return n; "
        "  end Bump; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var t := new Tiny(); "
        "    var i: IFive := t; "
        "    M.Bump(ref i.E); "
        "    if t._v = 5 then acc := acc + 1; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 1);
}

// ===========================================================================
// T5 — THE OVER-ACCEPTANCE / NON-REGRESSION CONTROLS. Narrowing an address
// resolution can only break things by declining a shape that DID need an
// address, so every neighbouring shape is pinned here. All were correct before
// the fix and must stay correct:
//   bit 1  a CLASS AUTO-property as a `ref` target — real backing storage, the
//          addressed path, and declared FIRST so its slot IS field index 0
//          (the exact shape a wrong "decline everything property-shaped" fix
//          would break);
//   bit 2  an interface property READ (`i.IP` in value position) — the itable
//          getter, never the address path;
//   bit 4  `i.IP := v` — routed to `lowerVirtualPropertySet` before
//          `lvalueAddr` since #2905;
//   bit 8  `i.IP += v` — the #481 read-modify-write arm;
//   bit 16 `i.IP++` — the #2858 arm, likewise ahead of `lvalueAddr`;
//   bit 32 a BLOCK-form interface property (`property C: int get; set; end;`)
//          as a `ref` target — `PropertyBodyKind::Block`, so it was NEVER
//          misclassified and must be unchanged;
//   bit 64 an ABSTRACT class property as a `ref` target through a base-typed
//          reference — the vtable sibling of the itable shape.
// Pre-fix: 127.  Post-fix: 127. A deliberate always-green control.
// ===========================================================================
BEZEL_TEST(codegen_interface_property_lvalue_addr_2931, neighbouring_shapes_unchanged_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2931e; "
        "public interface IHasQ "
        "  property IP: int; "
        "end IHasQ; "
        "public interface IBlock "
        "  property C: int "
        "    get; "
        "    set; "
        "  end; "
        "end IBlock; "
        "public class Box: IHasQ, IBlock "
        "  public property AP: int; "
        "  public _z: int := 0; "
        "  public _v: int := 0; "
        "  public _c: int := 0; "
        "  public property IP: int get => _v; set => do _v := value; end; end; "
        "  public property C: int get => _c; set => do _c := value; end; end; "
        "end Box; "
        "public abstract class Shape "
        "  public abstract property Name: int "
        "    get; "
        "    set; "
        "  end; "
        "end Shape; "
        "public class Circle: Shape "
        "  public _n0: int := 0; "
        "  public _n: int := 0; "
        "  public override property Name: int "
        "    get => _n; set => do _n := value; end; "
        "  end; "
        "end Circle; "
        "public class M "
        "  public static method Bump(ref n: int): int "
        "    n := n + 5; "
        "    return n; "
        "  end Bump; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b0 := new Box(); "
        "    M.Bump(ref b0.AP); "
        "    if b0.AP = 5 and b0._z = 0 and b0._v = 0 then acc := acc + 1; end; "
        "    using var b1 := new Box(); "
        "    b1._v := 7; "
        "    var i1: IHasQ := b1; "
        "    if i1.IP = 7 then acc := acc + 2; end; "
        "    using var b2 := new Box(); "
        "    var i2: IHasQ := b2; "
        "    i2.IP := 11; "
        "    if b2._v = 11 and b2._z = 0 then acc := acc + 4; end; "
        "    using var b3 := new Box(); "
        "    var i3: IHasQ := b3; "
        "    i3.IP += 4; "
        "    if b3._v = 4 and b3._z = 0 then acc := acc + 8; end; "
        "    using var b4 := new Box(); "
        "    var i4: IHasQ := b4; "
        "    i4.IP++; "
        "    if b4._v = 1 and b4._z = 0 then acc := acc + 16; end; "
        "    using var b5 := new Box(); "
        "    var i5: IBlock := b5; "
        "    M.Bump(ref i5.C); "
        "    if b5._c = 5 and b5._z = 0 and b5._v = 0 then acc := acc + 32; end; "
        "    using var c1 := new Circle(); "
        "    var s1: Shape := c1; "
        "    M.Bump(ref s1.Name); "
        "    if c1._n = 5 and c1._n0 = 0 then acc := acc + 64; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 127);
}
