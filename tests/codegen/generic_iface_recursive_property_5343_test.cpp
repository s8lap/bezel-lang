// Regression test for issue #5343 — §14.1 ("Generic declarations") / §23.2
// ("Properties and Indexers": expression-bodied property) / §12.1 ("Members
// are non-virtual by default" / override type identity), v0.313.
//
// THE DEFECT. A recursive expression-bodied property
// `override property Rec: T => (if _next = null then _v else _next!.Rec)`
// on a generic class `BoxM<T>` implementing an interface that declares `Rec`
// left the VTABLE's property-accessor slot pointing at the ERASED template
// accessor when the property's type is the type parameter itself. The template
// lowers `T` to the i32 placeholder, so its return type is i32; the `BoxM<long>`
// specialization returns i64. `specializedVtableFor`'s condition-(d) loop
// refused the specialization on a signature-ABI guard that compared the
// specialization against the TEMPLATE's type — the wrong reference — so the
// erased body stayed in a slot the read site calls as i64, and the program died
// with exit 139 in `bz$BoxM$Rec$propget`.
//
// THE DISCRIMINATOR IS NOT INSTANTIATION COUNT. Measured on the pre-fix tree:
// a program with a SINGLE `BoxM<long>` instantiation exits 139; the same source
// with a single `BoxM<int>` exits 0. The axis that decides is whether the
// property's declared type is the type parameter (so the specialization's
// return type differs from the erasure). The four rows below therefore vary
// INSTANTIATION COUNT on ONE generic class and hold everything else fixed —
// (a) one, (b) two, (c) three — plus (d) a control row that pins the VALUES, so
// a fix that cross-wires a slot between instantiations is caught by a wrong
// answer rather than passing silently.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Both oracles, always: `runAot` (plain AOT) and `runAotChecked` (the ASan
// link). A crash under one and not the other would be a harness artefact, so
// every row asserts both.
void expectBoth(const char* src, int expectedExit) {
    bezel::test::JitResult plain = runAot(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, expectedExit);
    bezel::test::JitResult checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, expectedExit);
}

// The class under test, shared verbatim by rows (a)-(d) so they differ ONLY in
// instantiation count. `Rec: T` is the load-bearing shape: the property's type
// IS the type parameter, so each instantiation's accessor has a different return
// type from the erased template's i32.
constexpr const char* kClassAndHeader = R"BZL(
module P;
interface IRecM
  readonly property Rec: int;
end IRecM;
class BoxM<T> : IRecM
  private _v: T;
  private _next: BoxM<T>?;
  public constructor(v: T, next: BoxM<T>?) _v := v; _next := next; end constructor;
  public override property Rec: T => (if _next = null then _v else _next!.Rec);
end BoxM;
)BZL";

std::string withMain(const std::string& body) {
    return std::string(kClassAndHeader) + body;
}

}  // namespace

// Row (a) — ONE instantiation. The control that proves the fix did not break the
// case that already worked. `BoxM<int>` is the one instantiation whose
// specialization happens to share the erasure's i32 return type, so it passed
// even pre-fix; it must still pass. Without this row a fix that, say, stopped
// building accessor specializations entirely would look green on (b) alone only
// if (b) were vacuous.
BEZEL_TEST(generic_iface_recursive_property_5343, one_instantiation_int) {
    expectBoth(withMain(R"BZL(
class C
  public static method Main(): int
    var bi := new BoxM<int>(1, new BoxM<int>(2, new BoxM<int>(3, null)));
    if bi.Rec <> 3 then return 1; end if;
    return 0;
  end Main;
end C;
)BZL").c_str(),
               0);
}

// Row (a') — ONE instantiation of a layout-MOVING type. This is the row that
// actually pins the defect: MEASURED pre-fix, this program exits 139 even
// though there is exactly ONE `BoxM` in it. It is the minimal repro of the
// issue's own reported shape and the reason the guard had to go.
BEZEL_TEST(generic_iface_recursive_property_5343, one_instantiation_long) {
    expectBoth(withMain(R"BZL(
class C
  public static method Main(): int
    var bl := new BoxM<long>(4L, new BoxM<long>(5L, new BoxM<long>(6L, null)));
    if bl.Rec <> 6L then return 2; end if;
    return 0;
  end Main;
end C;
)BZL").c_str(),
               0);
}

// Row (b) — TWO instantiations of the SAME class, differently-laid-out. The
// issue's original repro. Distinct values (3 vs 6) so a cross-wired slot shows
// up as a wrong answer, not merely as a crash.
BEZEL_TEST(generic_iface_recursive_property_5343, two_instantiations) {
    expectBoth(withMain(R"BZL(
class C
  public static method Main(): int
    var bi := new BoxM<int>(1, new BoxM<int>(2, new BoxM<int>(3, null)));
    var bl := new BoxM<long>(4L, new BoxM<long>(5L, new BoxM<long>(6L, null)));
    if bi.Rec <> 3 then return 1; end if;
    if bl.Rec <> 6L then return 2; end if;
    return 0;
  end Main;
end C;
)BZL").c_str(),
               0);
}

// Row (c) — THREE instantiations. If the defect were per-instantiation rather
// than about the erasure-vs-specialization signature, the third would be the
// one that fails; the row exists so a fix that only repairs the FIRST
// specialized slot cannot pass.
BEZEL_TEST(generic_iface_recursive_property_5343, three_instantiations) {
    expectBoth(withMain(R"BZL(
class C
  public static method Main(): int
    var bi := new BoxM<int>(1, new BoxM<int>(2, new BoxM<int>(3, null)));
    var bl := new BoxM<long>(4L, new BoxM<long>(5L, new BoxM<long>(6L, null)));
    var bd := new BoxM<double>(7.0, new BoxM<double>(8.0, new BoxM<double>(9.0, null)));
    if bi.Rec <> 3 then return 1; end if;
    if bl.Rec <> 6L then return 2; end if;
    if bd.Rec <> 9.0 then return 3; end if;
    return 0;
  end Main;
end C;
)BZL").c_str(),
               0);
}

// Row (d) — the MISDISPATCH CONTROL. This is the row a "does not segfault"
// test cannot replace. Removing a signature guard from vtable construction can
// silently install the WRONG FUNCTION in a slot: the program then runs, returns
// a plausible value, and is green in every ordinary test. Each instantiation
// here is seeded with a value that is unique ACROSS instantiations (30 / 300 /
// 9.0) and is checked against its OWN expected constant, so an accessor wired
// to another instantiation's slot returns a number that is wrong for it.
//
// The distinctness is the point: 30, 300 and 9.0 cannot be confused for one
// another, and a `BoxM<int>` accessor returning i32 where i64 was called
// truncates to a value no row above produces.
BEZEL_TEST(generic_iface_recursive_property_5343, misdispatch_control) {
    expectBoth(withMain(R"BZL(
class C
  public static method Main(): int
    var gi := new BoxM<int>(10, new BoxM<int>(20, new BoxM<int>(30, null)));
    var gl := new BoxM<long>(100L, new BoxM<long>(200L, new BoxM<long>(300L, null)));
    var gd := new BoxM<double>(1.0, new BoxM<double>(2.0, new BoxM<double>(9.0, null)));
    if gi.Rec <> 30 then return 100 + gi.Rec; end if;
    if gl.Rec <> 300L then return 200 + int(gl.Rec); end if;
    if gd.Rec <> 9.0 then return 300 + int(gd.Rec); end if;
    return 0;
  end Main;
end C;
)BZL").c_str(),
               0);
}

// Row (e) — ORDER. The same two instantiations as row (b) with the LONG one
// constructed and read FIRST. Establishes that the repair does not depend on
// which instantiation is emitted first, so a fix that merely happens to work
// for the emission order the compiler happens to pick is caught.
BEZEL_TEST(generic_iface_recursive_property_5343, two_instantiations_reversed) {
    expectBoth(withMain(R"BZL(
class C
  public static method Main(): int
    var bl := new BoxM<long>(4L, new BoxM<long>(5L, new BoxM<long>(6L, null)));
    var bi := new BoxM<int>(1, new BoxM<int>(2, new BoxM<int>(3, null)));
    if bl.Rec <> 6L then return 1; end if;
    if bi.Rec <> 3 then return 2; end if;
    return 0;
  end Main;
end C;
)BZL").c_str(),
               0);
}
