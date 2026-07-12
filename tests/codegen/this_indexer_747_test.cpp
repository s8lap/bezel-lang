// Spec-derived regression tests for fix #747 (R42 WRONG_VALUE) — a `this[i]`
// user-defined indexer access inside the declaring class's methods.
//
// Authority: §22.10 / §23.4. Bug: `this[i]` get/set/compound inside a class
// method silently no-op'd — `return this[0]` emitted `ret i32 0` and
// `this[0] := v` emitted no setter call. Root cause: lowerIndexerGet/Set/Compound
// resolved the receiver type via `ctx_.exprType(*target)`, but `this` carries no
// sema exprType (the member resolver uses the current type directly), so the
// indexer dispatch was skipped. The fix recovers the owner TypeSymbol from the
// current method for a `this`/`base` receiver (mirroring lowerWith). An external
// `c[i]` access already worked and is unaffected.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §22.10: `this[i] := v` then `return this[i]` inside class methods.
BEZEL_TEST(this_indexer_747, this_get_and_set) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Vec "
        "  public _d: Array<int> := [0, 0, 0]; "
        "  public property [i: int]: int "
        "    get => _d[i]; set => do _d[i] := value; end; end; "
        "  public method SetFirst(v: int) this[0] := v; end SetFirst; "
        "  public method GetFirst(): int return this[0]; end GetFirst; "
        "end Vec; "
        "class P public static method Main(): int "
        "  using var v := new Vec(); v.SetFirst(42); return v.GetFirst(); "
        "end Main; end P;"), 42);
}

// §22.10: `this[i] := this[i] + v` (compound through the self-indexer).
BEZEL_TEST(this_indexer_747, this_compound) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Vec "
        "  public _d: Array<int> := [10, 0, 0]; "
        "  public property [i: int]: int "
        "    get => _d[i]; set => do _d[i] := value; end; end; "
        "  public method AddFirst(v: int) this[0] := this[0] + v; end AddFirst; "
        "end Vec; "
        "class P public static method Main(): int "
        "  using var v := new Vec(); v.AddFirst(2); return v[0]; "
        "end Main; end P;"), 12);
}

// §22.10: an EXTERNAL `v[i]` access (the path that already worked) is unaffected.
BEZEL_TEST(this_indexer_747, external_indexer_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Vec "
        "  public _d: Array<int> := [0, 0, 0]; "
        "  public property [i: int]: int "
        "    get => _d[i]; set => do _d[i] := value; end; end; "
        "end Vec; "
        "class P public static method Main(): int "
        "  using var v := new Vec(); v[0] := 42; return v[0]; "
        "end Main; end P;"), 42);
}
