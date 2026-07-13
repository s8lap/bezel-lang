// Spec-derived tests for fix #741 — `Array<T>.Capacity` and the
// `constructor(capacity: int)` reservation.
//
// Authority: stdlib §4.4 `Array<T>` declares `public constructor(capacity: int)`
// and `public readonly property Capacity: int`; §4.5 `OwningArray<T>` mirrors it.
//
// Two defects fixed together here:
//   1. `.Capacity` had no codegen case, so the read returned an uninitialized
//      stack slot (garbage). It now reads the backing `bz_array_t.capacity`
//      via the `bz_array_capacity` runtime accessor.
//   2. The `constructor(capacity: int)` argument was ignored — every
//      Array/OwningArray/Stack/Queue/Set allocated a hardcoded capacity of 16,
//      so `new Array<int>(8).Capacity` could never reflect the request. The
//      arg is now honoured (reserving slots without setting Count), falling
//      back to the default for a non-positive request.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── §4.4 / #741: Capacity reflects the requested reservation ─────────────────

// new Array<int>(8) reserves capacity 8 (was: always 16, then read as garbage).
BEZEL_TEST(array_capacity_741, ctor_capacity_honoured) {
    // per stdlib §4.4
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(8); "
        "    return a.Capacity; "
        "  end Main; "
        "end C;"), 8);
}

// A larger reservation is honoured exactly, not clamped to a default.
BEZEL_TEST(array_capacity_741, ctor_capacity_large) {
    // per stdlib §4.4
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(1000); "
        "    Console.WriteLine(a.Capacity); return 0; "
        "  end Main; "
        "end C;"), "1000");
}

// The capacity ctor reserves space WITHOUT populating elements: Count is 0.
BEZEL_TEST(array_capacity_741, ctor_capacity_count_is_zero) {
    // per stdlib §4.4 — capacity is reserved space, not initial count
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(8); "
        "    return a.Count; "
        "  end Main; "
        "end C;"), 0);
}

// Add still grows past a small reservation (capacity 2, three Adds → Count 3).
BEZEL_TEST(array_capacity_741, add_grows_past_capacity) {
    // per stdlib §4.4
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(2); "
        "    a.Add(10); a.Add(20); a.Add(30); "
        "    return a.Count; "
        "  end Main; "
        "end C;"), 3);
}

// The no-arg ctor keeps the default capacity (16).
BEZEL_TEST(array_capacity_741, no_arg_default_capacity) {
    // per stdlib §4.4
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    return a.Capacity; "
        "  end Main; "
        "end C;"), 16);
}

// OwningArray<T> exposes Capacity over the same bz_array_t backing.
BEZEL_TEST(array_capacity_741, owned_array_capacity) {
    // per stdlib §4.5
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class Box public x: int := 0; end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new OwningArray<Box>(12); "
        "    return a.Capacity; "
        "  end Main; "
        "end C;"), 12);
}
