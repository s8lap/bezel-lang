// #5267 -- §22.3 / §28.3 (v0.313): a block-local `var value` that shadows an
// `owned` parameter `value` must not be confused with the parameter by the
// ownership pass. The inner `value` is moved into `Take`; the OUTER parameter
// is then stored into an owned field. Before the fix the pass marked the OUTER
// parameter moved-from and rejected the (valid) program with "owned value in
// `value` was already moved out". Loop-amplified 200x so the ASan leg pins that
// every Item is disposed exactly once.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

BEZEL_TEST(shadow_owned_param_by_symbol_5267, inner_moved_outer_stored_runs_clean_loop_spec_22_3) {
    const std::string src =
        "module M; "
        "class Item "
        "  public Id: int; "
        "  public static Disposed: int := 0; "
        "  public constructor(i: int) Id := i; end; "
        "  destructor Item.Disposed := Item.Disposed + 1; end destructor; "
        "end Item; "
        "class Helper "
        "  public static method Take(owned x: Item): int return x.Id; end Take; "
        "end Helper; "
        "class B "
        "  private owned _o: Item?; "
        "  public method SetV(owned value: Item) "
        "    do var value := new Item(9); Helper.Take(move value); end; "
        "    _o := value; "
        "  end SetV; "
        "  public method Last(): int return _o!.Id; end Last; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var b := new B(); "
        "      for var i in 0..<200 do b.SetV(new Item(i)); end; "
        "      Console.WriteLine(b.Last()); "
        "    end; "
        "    Console.WriteLine(Item.Disposed); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("199\n400") != std::string::npos);
}
