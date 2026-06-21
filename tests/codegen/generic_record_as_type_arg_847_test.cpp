// Codegen — issue #847: a non-generic multi-field RECORD used as a generic type
// ARGUMENT miscompiled in two related ways (§14.2 monomorphization / §9 records
// are value types):
//
//   Repro A — a `T?` field where T = a record returned garbage: the record's
//             fields were read at the wrong offsets (`p.X + p.Y` gave 20, not 30).
//   Repro B — a `T` return where T = a record emitted malformed IR:
//             "Function return type does not match operand type of return inst!
//              ret %record.Point %2 / %record.Point$spec0".
//
// ROOT CAUSE: recordStruct() specialized a record's layout whenever ANY generic
// substitution was active, minting a distinct `%record.Point$spec0` even though
// the non-generic `Point` has a fixed `[i32,i32]` layout that never depends on
// the substitution. The specialized return type then disagreed with the
// canonical `%record.Point` that the call-site argument / by-value field read
// already used. THE FIX gates the specialization branch on the record being
// ITSELF generic (`!t->genericParams.empty()`), mirroring classStruct — a
// non-generic record used as a type argument now reuses its canonical struct.
//
// The non-generic equivalents (a plain `Point?` field, a non-generic identity
// returning `Point`) already worked; these tests pin the generic forms.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Repro B: a generic identity method returning T, instantiated with a record.
// `return v` must keep the canonical record struct so the return type matches.
BEZEL_TEST(generic_record_as_type_arg_847, identity_returns_record_arg) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Identity<T>(v: T): T => v; "
        "  public static method Main(): int "
        "    var p := new Point(3, 4); "
        "    var p2 := Identity(p); "
        "    return p2.X + p2.Y; "  // 7
        "  end Main; "
        "end C;"), 7);
}

// Repro A: a generic value-class field `v: T?` where T = a record. Storing then
// reading back the record must read X/Y at the correct offsets.
BEZEL_TEST(generic_record_as_type_arg_847, nullable_record_field_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int); "
        "class Box<T> where T: value "
        "  public v: T?; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<Point>(); "
        "    b.v := new Point(10, 20); "
        "    var tmp: Point? := b.v; "
        "    if tmp is not null then "
        "      var p: Point := tmp!; "
        "      return p.X + p.Y; "  // 30
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 30);
}

// A second record instantiation in the SAME program (distinct field count) must
// also work — guards against the specialization cache reusing the wrong layout.
BEZEL_TEST(generic_record_as_type_arg_847, two_record_instantiations) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int); "
        "record Triple(A: int, B: int, C: int); "
        "class C "
        "  public static method Identity<T>(v: T): T => v; "
        "  public static method Main(): int "
        "    var p := Identity(new Point(3, 4)); "
        "    var t := Identity(new Triple(1, 2, 5)); "
        "    return p.X + p.Y + t.A + t.B + t.C; "  // 7 + 8 = 15
        "  end Main; "
        "end C;"), 15);
}
