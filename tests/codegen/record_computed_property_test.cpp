// Codegen — reading a record's computed (expression-bodied / block-form)
// property. issue #347.
//
// A record is a by-value LLVM struct (§9.4), not a heap pointer. Its computed
// property getter reads fields against `this` via a StructGEP, which requires
// a pointer base. Before the #347 fix the member-access lowering bound the
// record *value* as `this`, producing malformed IR (`getelementptr` on a
// non-pointer) that the backend rejects (verifier: "GEP base pointer is not a
// vector or a vector of pointers"; JIT/AOT crash). The fix spills the record
// value to a stack slot and binds that slot's address as `this`, mirroring the
// record field-access path (fieldAddr, CG29/#254) and the §9.2 record-storage
// access used by the synthesized ToString.
//
// Auto-property access (§9.2/CG33) and value semantics over auto-properties
// already worked; these tests cover only the computed-property getter path:
//  - §23.2 expression-bodied read-only property on a positional record;
//  - §23.3 block-form read-only property on a record;
//  - a computed property over a body record's auto-properties (§9.2);
//  - a computed property used inside an interpolated string (§18.5);
//  - a computed property that reads another computed property.
//
// Observed through the int exit code (or string Length for interpolation).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §23.2 / §9.3: an expression-bodied read-only property on a positional
// record. Reading `r.Sum` evaluates `X + Y` with `this` bound to the record's
// storage (3 + 4 == 7). per issue #347
BEZEL_TEST(codegen_record_computed_property, expr_bodied_positional_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int) "
        "  public property Sum: int => X + Y; "
        "end Point; "
        "class C public static method Main(): int "
        "  var r := new Point(3, 4); return r.Sum; "
        "end Main; end C;"), 7);
}

// §23.3 / §9.3: a block-form read-only property (`get => do ... end;`) on a
// record. The block getter is emitted as a standalone function taking a `this`
// pointer; the record value must be spilled so the pointer is well-formed
// (10 + 5 == 15). per issue #347
BEZEL_TEST(codegen_record_computed_property, block_bodied_record_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int) "
        "  public property Sum: int "
        "    get => do return X + Y; end; "
        "  end Sum; "
        "end Point; "
        "class C public static method Main(): int "
        "  var r := new Point(10, 5); return r.Sum; "
        "end Main; end C;"), 15);
}

// §9.2 body record: a computed property whose value is a pure function of the
// record's auto-properties (Width, Height). Constructed via `new ... with` and
// read through the computed `Area` (6 * 7 == 42). per issue #347
BEZEL_TEST(codegen_record_computed_property, computed_over_body_record_autoprops) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Rect "
        "  public property Width: int; "
        "  public property Height: int; "
        "  public property Area: int => Width * Height; "
        "end Rect; "
        "class C public static method Main(): int "
        "  var r := new Rect() with(Width := 6, Height := 7); "
        "  return r.Area; "
        "end Main; end C;"), 42);
}

// §18.5: a record computed property read inside an interpolated string.
// `$"sum={r.Sum}"` is "sum=7" (length 5); exercises the same member-access
// lowering through the interpolation path. per issue #347
BEZEL_TEST(codegen_record_computed_property, computed_property_in_interpolation) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int) "
        "  public property Sum: int => X + Y; "
        "end Point; "
        "class C public static method Main(): int "
        "  var r := new Point(3, 4); "
        "  var s := $\"sum={r.Sum}\"; return s.Length; "
        "end Main; end C;"), 5);
}

// §23.2: a computed property whose getter reads another computed property of
// the same record. The inner read uses the implicit `this` (already a pointer
// inside the spilled getter), so the result composes (Sum = 7, Doubled = 14).
// per issue #347
BEZEL_TEST(codegen_record_computed_property, computed_property_reads_computed_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int) "
        "  public property Sum: int => X + Y; "
        "  public property Doubled: int => Sum + Sum; "
        "end Point; "
        "class C public static method Main(): int "
        "  var r := new Point(3, 4); return r.Doubled; "
        "end Main; end C;"), 14);
}
