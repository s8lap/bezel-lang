// fix #899 / §28.8.1: `using var x := move _f` (BARE form) inside a destructor
// must dispose the owned field exactly once — no leak.
//
// The #825 fix added a null-store for a bare `move _field` to prevent a
// double-free in the normal (non-destructor) case.  Without the #899 guard,
// that null-store also fired inside a destructor, zeroing the field slot before
// the auto-dispose pass (emitOwnedFieldDisposals) ran, so the auto-dispose
// skipped it (null-guarded) and the `using var` scope-end disposal was also
// suppressed (emittingDisposerBody_) → net result: zero disposals, i.e. a LEAK.
//
// Fix: guard the bare-form null-store on !emittingDisposerBody_, mirroring the
// qualified-form fix landed in #897/#898.
//
// Test matrix:
//   1. dtor bare-move (`using var x := move _f`)           → disposed exactly once
//   2. dtor qualified-move (`using var x := move this._f`) → disposed exactly once (regression)
//   3. non-dtor bare-move in a method (Swap pattern)       → nulls slot; no double-free
//   4. dtor with no move at all                            → auto-disposed once (baseline)
//   5. form-parity: bare and qualified in separate classes → each disposes once

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §28.8.1, #899: bare `using var x := move _f` in a destructor disposes once
// (not zero, i.e. no leak).
BEZEL_TEST(dtor_bare_move_dispose_899, bare_move_in_dtor_disposes_once) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Resource "
        "  public static _disposed: int := 0; "
        "  destructor Resource._disposed := Resource._disposed + 1; end destructor; "
        "end Resource; "
        "class Owner "
        "  private owned _res: Resource; "
        "  public constructor() this._res := new Resource(); end constructor; "
        "  destructor "
        "    using var taken := move _res; "
        "  end destructor; "
        "end Owner; "
        "class Program "
        "  public static method Main(): int "
        "    do using var o := new Owner(); end; "
        "    return Resource._disposed; "
        "  end Main; "
        "end Program;"), 1);
}

// §28.8.1, #897/#898 regression: qualified `using var x := move this._f` in a
// destructor still disposes once (must not regress after #899 change).
BEZEL_TEST(dtor_bare_move_dispose_899, qualified_move_in_dtor_disposes_once) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Resource "
        "  public static _disposed: int := 0; "
        "  destructor Resource._disposed := Resource._disposed + 1; end destructor; "
        "end Resource; "
        "class Owner "
        "  private owned _res: Resource; "
        "  public constructor() this._res := new Resource(); end constructor; "
        "  destructor "
        "    using var taken := move this._res; "
        "  end destructor; "
        "end Owner; "
        "class Program "
        "  public static method Main(): int "
        "    do using var o := new Owner(); end; "
        "    return Resource._disposed; "
        "  end Main; "
        "end Program;"), 1);
}

// §28.8.1, #825 regression: bare `move _f` in a REGULAR METHOD (not a
// destructor) still nulls the field slot so the owner's auto-dispose skips the
// already-moved object.  Two disposals expected: one inside Swap (do-block
// disposes the moved-out resource) and one in the destructor (the replacement
// resource installed by Swap).
BEZEL_TEST(dtor_bare_move_dispose_899, non_dtor_bare_move_still_nulls_slot) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Resource "
        "  public static _disposed: int := 0; "
        "  destructor Resource._disposed := Resource._disposed + 1; end destructor; "
        "end Resource; "
        "class Owner "
        "  public owned _res: Resource; "
        "  public constructor() this._res := new Resource(); end constructor; "
        "  public method Swap() "
        "    do using var taken := move _res; end; "
        "    _res := new Resource(); "
        "  end Swap; "
        "  destructor "
        "  end destructor; "
        "end Owner; "
        "class Program "
        "  public static method Main(): int "
        "    do using var o := new Owner(); "
        "      o.Swap(); "
        "    end; "
        "    return Resource._disposed; "
        "  end Main; "
        "end Program;"), 2);
}

// §28.8.1 baseline: destructor that does NOT move the field — field auto-disposed
// exactly once.
BEZEL_TEST(dtor_bare_move_dispose_899, no_move_in_dtor_auto_disposes_once) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Resource "
        "  public static _disposed: int := 0; "
        "  destructor Resource._disposed := Resource._disposed + 1; end destructor; "
        "end Resource; "
        "class Owner "
        "  private owned _res: Resource; "
        "  public constructor() this._res := new Resource(); end constructor; "
        "  destructor "
        "  end destructor; "
        "end Owner; "
        "class Program "
        "  public static method Main(): int "
        "    do using var o := new Owner(); end; "
        "    return Resource._disposed; "
        "  end Main; "
        "end Program;"), 1);
}

// §28.8.1, #899 form-parity: bare and qualified forms inside separate
// destructors must behave identically — each disposes its owned field once,
// for a total of 2 disposals.
BEZEL_TEST(dtor_bare_move_dispose_899, bare_and_qualified_form_parity) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Resource "
        "  public static _disposed: int := 0; "
        "  destructor Resource._disposed := Resource._disposed + 1; end destructor; "
        "end Resource; "
        "class OwnerBare "
        "  private owned _res: Resource; "
        "  public constructor() this._res := new Resource(); end constructor; "
        "  destructor "
        "    using var taken := move _res; "
        "  end destructor; "
        "end OwnerBare; "
        "class OwnerQualified "
        "  private owned _res: Resource; "
        "  public constructor() this._res := new Resource(); end constructor; "
        "  destructor "
        "    using var taken := move this._res; "
        "  end destructor; "
        "end OwnerQualified; "
        "class Program "
        "  public static method Main(): int "
        "    do using var a := new OwnerBare(); end; "
        "    do using var b := new OwnerQualified(); end; "
        "    return Resource._disposed; "
        "  end Main; "
        "end Program;"), 2);
}
