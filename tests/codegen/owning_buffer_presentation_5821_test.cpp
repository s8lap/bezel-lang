// Fix #5821 (probe) — §14.2 two-presentation declarations: the ACCEPT row.
// Spec: §14.2 *Two-presentation declarations* (v0.338); stdlib §4.14 (v0.338),
// `OwningBuffer<T>` value-typed presentation.
//
// A generic body that names `OwningBuffer<T>` at a type parameter is checked
// once against the members both presentations share; at an instantiation whose
// argument selects the value presentation with a defaultable argument (`int`),
// the one-argument constructor is admitted. This pins the accept row of the
// issue's case list, which already conforms on the base revision.
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// §14.2 two-presentation / §4.14: `Buf2735<int>` — the value presentation at a
// defaultable argument is admitted and the constructor fills `Length` slots.
BEZEL_TEST(owning_buffer_presentation_5821, spec_14_2_two_presentation_accept_int) {
    auto output = runAotOut(
        "module M; "
        "class Buf2735<T> "
        "  private owned _cells: OwningBuffer<T>; "
        "  public constructor(n: int) _cells := new OwningBuffer<T>(n); end constructor; "
        "  public method Count(): int => _cells.Length; "
        "end Buf2735; "
        "class Main public static method Main(): int "
        "  using var g := new Buf2735<int>(4); "
        "  Console.WriteLine(g.Count()); return 0; "
        "end Main; end Main;");
    BEZEL_EXPECT_EQ(output, "4");
}
