// Regression tests for #6910 (spec v0.340): a §21.10 LOCAL record's interface
// list was never resolved, so `record Sq(S: int): IShape` inside a method did
// not implement `IShape` -- the conversion to the interface and
// `Array<W>.Sort()`'s `T: IComparable<T>` requirement (§14.2) were both
// rejected. §21.10: "interface implementation (§9.6) ... computed by their
// existing rules from the declaration". Top-level rows are the controls.
//
// ORACLE: runAotChecked (real AOT path, generated-code ASan). Results are
// small exit codes (< 256).

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// The record body is shared by the local and the top-level spelling so the
// matrix compares the same declaration at two positions.
const char* kRec =
    "record W(v: int): IShape, IComparable<W> "
    "  public method Area(): int return v * v; end Area; "
    "  public method CompareTo(other: W): int return v - other.v; end CompareTo; "
    "end W; ";
const char* kPre =
    "module M; "
    "interface IShape method Area(): int; end IShape; ";
// Two bodies, run apart: boxing a record into an interface after other locals
// inside a GENERIC method leaks one box even for a top-level record (#6929), so
// the interface body and the Sort body are not combined.
const char* kBodyIface =
    "    var q := new W(4); "
    "    var s: IShape := q; "
    "    return s.Area() + s.Area(); ";  // 32
const char* kBodySort =
    "    var xs := new Array<W>(); "
    "    xs.Add(new W(3)); xs.Add(new W(1)); xs.Add(new W(2)); "
    "    xs.Sort(); "
    "    return xs[0].v * 10 + xs[2].v; ";  // 13

std::string prog(bool local, bool generic, const char* body) {
    std::string p = kPre;
    if (!local) p += kRec;
    p += "class C ";
    p += generic ? "  public static method Run<T>(t: T): int "
                 : "  public static method Run(): int ";
    if (local) p += kRec;
    p += body;
    p += "  end Run; ";
    p += "  public static method Main(): int ";
    p += generic ? "    return Run<int>(1); " : "    return Run(); ";
    p += "  end Main; end C; ";
    return p;
}
}  // namespace

// #6910 rows: {local, top-level} x {non-generic, generic enclosing} x
// {interface conversion, IComparable<W> Sort}.
#define ROW(name, local, generic, body, want)                                 \
    BEZEL_TEST(local_record_interface_6910, name) {                           \
        BEZEL_EXPECT_EQ(run(prog(local, generic, body)), want);               \
    }
ROW(spec_21_10_local_iface_nongeneric, true, false, kBodyIface, 32)
ROW(spec_21_10_local_iface_generic_enclosing, true, true, kBodyIface, 32)
ROW(spec_9_6_top_level_iface_nongeneric_control, false, false, kBodyIface, 32)
ROW(spec_9_6_top_level_iface_generic_enclosing_control, false, true, kBodyIface, 32)
ROW(spec_21_10_local_sort_nongeneric, true, false, kBodySort, 13)
ROW(spec_21_10_local_sort_generic_enclosing, true, true, kBodySort, 13)
ROW(spec_14_2_top_level_sort_nongeneric_control, false, false, kBodySort, 13)
ROW(spec_14_2_top_level_sort_generic_enclosing_control, false, true, kBodySort, 13)

// The issue's call-argument conversion (`Show(q)`), non-generic.
BEZEL_TEST(local_record_interface_6910, spec_21_10_local_record_as_interface_call_argument) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface IShape method Area(): int; end IShape; "
        "class C "
        "  public static method Show(s: IShape): int return s.Area(); end Show; "
        "  public static method Main(): int "
        "    record Sq(S: int): IShape "
        "      public method Area(): int return S * S; end Area; "
        "    end Sq; "
        "    var q := new Sq(4); "
        "    return Show(q); "
        "  end Main; end C; "), 16);
}

// A local record that names an interface it does not implement is still
// rejected, as the top-level twin is (§12.5).
BEZEL_TEST(local_record_interface_6910, spec_21_10_missing_member_still_rejected) {
    auto r = runAotChecked(
        "module M; interface IShape method Area(): int; end IShape; "
        "class C public static method Main(): int "
        "  record Sq(S: int): IShape end Sq; "
        "  var s: IShape := new Sq(4); return 0; end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

