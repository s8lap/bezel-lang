// Codegen — issue #7960: a `using var` (or plain `var`) bound to a `move X`
// of an iterator-format ENUMERATOR is the single disposer of the moved value
// (spec §28.8.1 / §28.3 / §28.6 / §29.7, v0.357).
//
// THE DEFECT (measured on staging e49c32aca). `move h.En!` / `move e` hand the
// enumerator frame to the destination and leave the SOURCE moved-from (§28.8.1,
// §28.3), so the source's own disposal is skipped. The destination's
// registration (registerUsingDisposal) only treated a producer CALL, a `new`,
// or a minted wrapper as an ownership source, so the `move` initializer
// registered NO disposal: the frame leaked and the generator's `finally` never
// ran (§29.7: disposing an iterator enumerator runs its pending `finally`).
//
// Oracle: runAotChecked (ASan+LSan) for leak/double-free, and the program's
// stdout for the `finally` ("fin") count, so a fix that reclaimed the frame
// without running Dispose (or that disposed twice) is still red.
//
// Spec: §28.8.1 (move), §28.3 (affine ownership), §28.6, §29.7 (enumerator
// Dispose runs pending `finally`). Spec revision v0.357.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

const char* kGen =
    "class G "
    "  public static method Gen(): IEnumerable<int> "
    "    try yield return 1; yield return 2; "
    "    finally Console.WriteLine(\"fin\"); end; "
    "  end Gen; "
    "end G; ";

std::string rep(const std::string& unit, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += unit;
    return out;
}

}  // namespace

// The issue's first repro: `using var m := move h.En!` over an `owned`
// enumerator FIELD, 50 iterations. Each iteration prints "moved" then, at the
// scope end, the `finally` ("fin") exactly once.
BEZEL_TEST(codegen_using_var_move_enumerator_7960,
           issue_shape_move_owned_field_into_using_var_spec_28_8_1) {
    std::string src =
        std::string("module M; ") + kGen +
        "class H "
        "  public owned En: IEnumerator<int>? := null; "
        "  public method Fill() En := G.Gen().GetEnumerator(); end Fill; "
        "end H; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 50 do "
        "    using var h := new H(); h.Fill(); var ok := h.En!.MoveNext(); "
        "    using var m := move h.En!; "
        "    ok := m.MoveNext(); Console.WriteLine(\"moved\"); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("moved\nfin\n", 50) + "end\n");
}

// The issue's second repro: `using var m := move e` of a LOCAL enumerator.
BEZEL_TEST(codegen_using_var_move_enumerator_7960,
           issue_shape_move_local_into_using_var_spec_28_8_1) {
    std::string src =
        std::string("module M; ") + kGen +
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 50 do "
        "    var e := G.Gen().GetEnumerator(); "
        "    var ok := e.MoveNext(); "
        "    using var m := move e; "
        "    ok := m.MoveNext(); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("fin\n", 50) + "end\n");
}

// The plain-`var` spelling of the same move (the repro 7960c): the destination
// is the sole owner, so it is dropped at scope end (§28.3).
BEZEL_TEST(codegen_using_var_move_enumerator_7960,
           plain_var_move_owned_field_spec_28_3) {
    std::string src =
        std::string("module M; ") + kGen +
        "class H "
        "  public owned En: IEnumerator<int>? := null; "
        "  public method Fill() En := G.Gen().GetEnumerator(); end Fill; "
        "end H; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 50 do "
        "    using var h := new H(); h.Fill(); var ok := h.En!.MoveNext(); "
        "    var m := move h.En!; "
        "    ok := m!.MoveNext(); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("fin\n", 50) + "end\n");
}

// ADMITTING TWIN / control: a `using var` bound to a fresh PRODUCER call (not a
// move) was already disposed exactly once before the fix and must stay so.
BEZEL_TEST(codegen_using_var_move_enumerator_7960,
           control_using_var_fresh_producer_spec_29_7) {
    std::string src =
        std::string("module M; ") + kGen +
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 50 do "
        "    using var m := G.Gen().GetEnumerator(); "
        "    var ok := m.MoveNext(); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("fin\n", 50) + "end\n");
}

// RAW USER CLASS in the moved slot (review r42-rev-c14-7960, probes p7, p7b,
// p13). §8.1 lets a user class implement `IEnumerator<int>`; such a value
// reaches an interface-typed slot as a class instance (class vtable, Dispose at
// slot 2, §28.2), NOT as an iterator frame (slot 1, §29.2). The `move`
// destination owns it (§28.8.1) and must dispose it exactly once, by the
// value's own representation. A blind slot-1 dispatch SEGVs in `itmap$M.MyE`
// at the first scope end (MEASURED on the first revision of this fix); the
// pre-fix compiler leaked it without crashing. Each iteration's destructor
// prints "mydisp" exactly once.
namespace {

const char* kMyE =
    "class MyE: IEnumerator<int> "
    "  private i: int := 0; "
    "  public method MoveNext(): bool "
    "    if this.i < 2 then this.i := this.i + 1; return true; end if; "
    "    return false; "
    "  end MoveNext; "
    "  public property Current: int get => this.i; end Current; "
    "  destructor Console.WriteLine(\"mydisp\"); end destructor; "
    "end MyE; ";

}  // namespace

// Probe p7: `using var m := move e;` over a local holding `new MyE()`.
BEZEL_TEST(codegen_using_var_move_enumerator_7960,
           raw_user_class_move_local_into_using_var_spec_28_8_1) {
    std::string src =
        std::string("module M; ") + kMyE +
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    var e: IEnumerator<int> := new MyE(); "
        "    var ok := e.MoveNext(); "
        "    using var m := move e; "
        "    ok := m.MoveNext(); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("mydisp\n", 100) + "end\n");
}

// Probe p7b: the plain-`var` spelling, `var m := move e;` (§28.3 auto-drop).
BEZEL_TEST(codegen_using_var_move_enumerator_7960,
           raw_user_class_move_local_into_plain_var_spec_28_3) {
    std::string src =
        std::string("module M; ") + kMyE +
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    var e: IEnumerator<int> := new MyE(); "
        "    var ok := e.MoveNext(); "
        "    var m := move e; "
        "    ok := m.MoveNext(); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("mydisp\n", 100) + "end\n");
}

// Probe p13: `using var m := move h.En!;` taking an `owned` enumerator FIELD
// that holds `new MyE()`.
BEZEL_TEST(codegen_using_var_move_enumerator_7960,
           raw_user_class_move_owned_field_into_using_var_spec_28_8_1) {
    std::string src =
        std::string("module M; ") + kMyE +
        "class H "
        "  public owned En: IEnumerator<int>? := null; "
        "  public method Fill() this.En := new MyE(); end Fill; "
        "end H; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    using var h := new H(); h.Fill(); var ok := h.En!.MoveNext(); "
        "    using var m := move h.En!; "
        "    ok := m.MoveNext(); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("mydisp\n", 100) + "end\n");
}

// The three spellings above call `MoveNext()` through the interface-typed slot,
// and on this tree that call itself reaches the class's Dispose (a separate,
// pre-existing dispatch defect filed alongside this fix), which would print
// "mydisp" whether or not the scope-end disposal ran. These rows (probe p12's
// shape) make no call through the slot, so the only "mydisp" each iteration can
// print is the destination's own scope-end disposal (§28.8.1 / §28.3).
BEZEL_TEST(codegen_using_var_move_enumerator_7960,
           raw_user_class_move_local_into_using_var_no_call_spec_28_8_1) {
    std::string src =
        std::string("module M; ") + kMyE +
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    var e: IEnumerator<int> := new MyE(); "
        "    using var m := move e; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("mydisp\n", 100) + "end\n");
}

BEZEL_TEST(codegen_using_var_move_enumerator_7960,
           raw_user_class_move_local_into_plain_var_no_call_spec_28_3) {
    std::string src =
        std::string("module M; ") + kMyE +
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    var e: IEnumerator<int> := new MyE(); "
        "    var m := move e; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("mydisp\n", 100) + "end\n");
}

BEZEL_TEST(codegen_using_var_move_enumerator_7960,
           raw_user_class_move_owned_field_into_using_var_no_call_spec_28_8_1) {
    std::string src =
        std::string("module M; ") + kMyE +
        "class H "
        "  public owned En: IEnumerator<int>? := null; "
        "  public method Fill() this.En := new MyE(); end Fill; "
        "end H; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    using var h := new H(); h.Fill(); "
        "    using var m := move h.En!; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("mydisp\n", 100) + "end\n");
}
