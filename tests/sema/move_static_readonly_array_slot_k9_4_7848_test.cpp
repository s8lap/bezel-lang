// fix #7848 (spec §28.8.1, §22.12 note (h), Kernel K9.4, v0.350): a `move` of a
// slot of a `static readonly` owning inline array is a compile error naming
// K9.4. The slots of such a field are static owned storage -- the always-live
// source, disposed only by §22.13's exit pass -- so they are "neither taken nor
// replaced". Before the fix `return move Registry.Pool[0];` compiled with only
// the declaration-site warning, and a program that ran it handed the caller an
// owner of an object the exit pass would dispose again.
//
// Rows:
//   T1  the bare take through the qualified spelling `Registry.Pool[0]`
//   T2  the bare identifier spelling inside the declaring class (`Pool[0]`)
//   T3  the null-forgiving spelling `move Registry.Pool[0]!`
//   T4  the replacing spelling of corpus case s28_8_1_0029 (`... with new Res()`)
//       also names K9.4 (the exchange itself stays declined, issue #5923)
//   T5  ACCEPT controls: an instance owning-array slot and a local owning-array
//       slot keep their `move` (K9.4 is about static storage only).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
const char* kRes =
    "module M7848; "
    "class Res "
    "  public constructor() end constructor; "
    "end Res; ";
}  // namespace

BEZEL_TEST(sema_move_static_readonly_array_slot_7848,
           qualified_bare_take_names_k9_4_spec_28_8_1) {
    auto r = runAot(std::string(kRes) +
        "class Registry "
        "  public static readonly Pool: [2 owned]Res?; "
        "  public static method Swap(): owned Res? "
        "    return move Registry.Pool[0]; "
        "  end Swap; "
        "end Registry; "
        "class P public static method Main() "
        "  using var r := Registry.Swap(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "K9.4"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "Registry.Pool"));
}

BEZEL_TEST(sema_move_static_readonly_array_slot_7848,
           bare_identifier_take_names_k9_4_spec_28_8_1) {
    auto r = runAot(std::string(kRes) +
        "class Registry "
        "  public static readonly Pool: [2 owned]Res?; "
        "  public static method Swap(): owned Res? "
        "    return move Pool[1]; "
        "  end Swap; "
        "end Registry; "
        "class P public static method Main() "
        "  using var r := Registry.Swap(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "K9.4"));
}

BEZEL_TEST(sema_move_static_readonly_array_slot_7848,
           null_forgiving_take_names_k9_4_spec_28_8_1) {
    auto r = runAot(std::string(kRes) +
        "class Registry "
        "  public static readonly Pool: [2 owned]Res?; "
        "  public static method Swap(): owned Res "
        "    return move Registry.Pool[0]!; "
        "  end Swap; "
        "end Registry; "
        "class P public static method Main() "
        "  using var r := Registry.Swap(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "K9.4"));
}

BEZEL_TEST(sema_move_static_readonly_array_slot_7848,
           replacing_spelling_names_k9_4_corpus_s28_8_1_0029) {
    auto r = runAot(std::string(kRes) +
        "class Registry "
        "  public static readonly Pool: [2 owned]Res?; "
        "  public static method Swap(): owned Res? "
        "    return move Registry.Pool[0] with new Res(); "
        "  end Swap; "
        "end Registry; "
        "class P public static method Main() "
        "  using var r := Registry.Swap(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "K9.4"));
}

// ACCEPT controls: the rejection is about STATIC storage. An instance field's
// owning-inline-array slot and a local's slot still take (the replacing take is
// §28.8.1's bare `move arr[i]`, which stores `null` at a `[N owned]T?` slot).
BEZEL_TEST(sema_move_static_readonly_array_slot_7848,
           instance_and_local_owning_array_slot_take_still_accepted) {
    auto r = runAot(
        "module M7848C; "
        "class Res "
        "  public Name: string; "
        "  public constructor(n: string) this.Name := n; end constructor; "
        "end Res; "
        "class Holder "
        "  public Slots: [2 owned]Res?; "
        "  public constructor() "
        "    this.Slots[0] := new Res(\"a\"); "
        "    this.Slots[1] := new Res(\"b\"); "
        "  end constructor; "
        "  public method Take(): owned Res? "
        "    return move this.Slots[0]; "
        "  end Take; "
        "end Holder; "
        "class P public static method Main() "
        "  using var h := new Holder(); "
        "  using var t := h.Take(); "
        "  var loc: [2 owned]Res? := [new Res(\"x\"), new Res(\"y\")]; "
        "  using var u := move loc[1]; "
        "  Console.WriteLine($\"{t!.Name} {u!.Name}\"); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a y\n"));
}
