// fix #7124 / §22.3 *Parameter modifiers* (spec v0.359): the `move` call-site
// marker is valid only on an argument to an `owned` parameter position, in
// every spelling of the call. The spec: "writing it on a borrow, a fresh
// producer, or a non-`owned` parameter position is likewise an error".
//
// MEASURED on b279e8daf: the direct `s.Accept(move d)` over a borrow parameter
// is rejected, while `f(move d)` through a function value `f: (D) -> ()` was
// ACCEPTED (and leaked `d`). A function value's parameter slots, modifiers
// included, are part of its TYPE (§27.1), so the bound slot is read from the
// callee's function type. The fix is a REJECTION with the direct path's own
// wording; the function-value path is not taught to honour `move` at a
// non-`owned` slot.
//
// Each row is red on the base. The admitting twins (an `(owned D) -> ()` slot,
// a plain borrow argument, a fresh producer) are corpus candidates.
//
// per §22.3, §27.1, v0.359, #7124

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

constexpr const char* kMarker = "the `move` call-site marker is only valid";

const char* kD = R"(
module M;
class D
  public constructor() end constructor;
end D;
class H
  public cb: (D) -> () := lambda(x: D) => do end;
end H;
)";

// ROW 1 — the issue's repro: a bare statement call.
BEZEL_TEST(function_value_move_marker_non_owned_slot_7124,
           bare_statement_call_rejected_spec_22_3_v0_359) {
    std::string src = std::string(kD) + R"(
class Program
  public static method Main(): int
    var d := new D();
    var f: (D) -> () := lambda(x: D) => do end;
    f(move d);
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMarker));
}

// ROW 2 — the local-declaration initializer and assignment-RHS walk, which is
// a different code path from the bare-statement one.
BEZEL_TEST(function_value_move_marker_non_owned_slot_7124,
           local_initializer_call_rejected_spec_22_3_v0_359) {
    std::string src = std::string(kD) + R"(
class Program
  public static method Main(): int
    var d := new D();
    var f: (D) -> int := lambda(x: D) => 1;
    var r := f(move d);
    return r - 1;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMarker));
}

// ROW 3 — a function-typed FIELD as the callee.
BEZEL_TEST(function_value_move_marker_non_owned_slot_7124,
           field_callee_rejected_spec_22_3_v0_359) {
    std::string src = std::string(kD) + R"(
class Program
  public static method Main(): int
    var h := new H();
    var d := new D();
    h.cb(move d);
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMarker));
}

}  // namespace
