// Regression tests for issue #4918 — §13 / §14.1 / §7.10: a NON-generic
// class implementing a generic interface at a nullable VALUE type
// (`class K: IId<int?>`) with the member declared directly as `int?`.
//
// `K.Id(v: int?): int?` implements `IId<T>.Id(v: T): T` exactly at
// `T := int?`; no nullability widening is involved. The itable builder
// compared against the ERASED interface signature (bare `T` → the i32
// placeholder), took `i32` vs `{i8, i32}` for a §11.2 widening and emitted a
// widening thunk that returned the `{i8, i32}` carrier from an `i32`
// function — bezelc: IR verification failed.
//
// spec §13 (interface implementation), §14.1 (monomorphization substitutes
// the supplied type argument), §7.10, §11.2, v0.307.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints4918(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §13 / §14.1 (v0.307), fix #4918 — the issue's repro verbatim, through
// the interface (present) and directly on the class (null).
BEZEL_TEST(iface_generic_nullable_value_impl_4918, issue_repro_iid_int_nullable_13_14_1) {
    expectPrints4918(
        "module M; "
        "public interface IId<T> "
        "  method Id(v: T): T; "
        "end IId; "
        "public class K: IId<int?> "
        "  public constructor() end constructor; "
        "  public method Id(v: int?): int? "
        "    return v; "
        "  end Id; "
        "end K; "
        "public class C "
        "  public static method Main() "
        "    using var k := new K(); "
        "    var i: IId<int?> := k; "
        "    Console.WriteLine(i.Id(5) ?? -1); "
        "    Console.WriteLine(k.Id(null) ?? -1); "
        "  end Main; "
        "end C;",
        "5\n-1\n");
}

// per §13 / §14.1 (v0.307), fix #4918 — null through the interface, a
// `long?` payload with a second parameter, and 50 interface calls in a loop.
// The loop assigns a `long` local (`kl`), not the `int` loop variable: an
// `int` → `long?` re-assignment (`x := k`) silently stores 0 (#5005, a
// separate pre-existing defect), which is not what this row pins.
BEZEL_TEST(iface_generic_nullable_value_impl_4918, null_long_two_params_and_loop_13_14_1) {
    expectPrints4918(
        "module M; "
        "public interface IOr<T> "
        "  method Or(v: T, d: T): T; "
        "end IOr; "
        "public class L: IOr<long?> "
        "  public constructor() end constructor; "
        "  public method Or(v: long?, d: long?): long? "
        "    return v ?? d; "
        "  end Or; "
        "end L; "
        "public class C "
        "  public static method Main() "
        "    using var l := new L(); "
        "    var i: IOr<long?> := l; "
        "    var n: long? := null; var a: long? := -9; var s: long? := 7; "
        "    Console.WriteLine(i.Or(n, s) ?? -1); "
        "    Console.WriteLine(i.Or(n, n) ?? -1); "
        "    Console.WriteLine(i.Or(a, s) ?? -1); "
        "    var sum: long := 0; "
        "    for var k in 0..<50 do "
        "      var x: long? := null; var one: long? := 1; "
        "      var kl: long := k; "
        "      if k mod 2 = 0 then x := kl; end; "
        "      sum := sum + (i.Or(x, one) ?? 0); "
        "    end; "
        "    Console.WriteLine(sum); "
        "  end Main; "
        "end C;",
        "7\n-1\n-9\n625\n");
}
