// fix #763 / §28.8.1: moving an owned field out inside a destructor and
// discharging the taken value disposes that value EXACTLY ONCE — no leak (the
// field's auto-dispose pass skips the moved-from field) and no double-free.
//
// §28.8.1: "`move x.f` inside a destructor … causes the compiler-generated
// auto-dispose pass for that field to skip it (the field is in moved-from state
// and has nothing to dispose). The taken value must still be discharged …"
//
// These run the program end to end (the destructor actually fires) and count
// Inner disposals, locking in the runtime soundness that the diagnostic fix
// (tests/ownership/destructor_field_move_763_test.cpp) depends on.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §28.8.1: `var taken := move this._inner; taken.Dispose();` — Inner disposed
// exactly once.
BEZEL_TEST(destructor_field_move_runtime_763, move_then_explicit_dispose_disposes_once) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Inner public static disposed: int := 0; "
        "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
        "end Inner; "
        "class Outer "
        "  private owned _inner: Inner; "
        "  public constructor() this._inner := new Inner(); end constructor; "
        "  destructor "
        "    var taken := move this._inner; "
        "    taken.Dispose(); "
        "  end destructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  do using var o := new Outer(); end; "
        "  return Inner.disposed; "
        "end Main; end C;"), 1);
}

// §28.8.1: the canonical `using var u := move this._inner;` — also exactly once.
BEZEL_TEST(destructor_field_move_runtime_763, move_into_using_var_disposes_once) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Inner public static disposed: int := 0; "
        "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
        "end Inner; "
        "class Outer "
        "  private owned _inner: Inner; "
        "  public constructor() this._inner := new Inner(); end constructor; "
        "  destructor "
        "    using var u := move this._inner; "
        "  end destructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  do using var o := new Outer(); end; "
        "  return Inner.disposed; "
        "end Main; end C;"), 1);
}
