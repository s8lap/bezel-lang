// §28.3 *Factory methods and sourceless returns* / §11.8 / §9.7 (v0.338; issue
// #5818): a static method implementing a static interface member declared with
// a bare-`T` return and a non-empty source set takes the announcement table's
// row 5. Its return paths are checked against the positionally corresponding
// source parameters, and §9.7's return bullet reads it at a conformer that holds
// borrows: `return a;` in `Pair.Pick(a: Pair, b: Pair): Pair` over
// `record Pair(It: Item): IPick<Pair>` is admitted (§28.3 note 1, v0.338).
//
// This is the RECORD-conformer example from §28.3 note 1, verbatim in shape. The
// caller's result is kept within `a`'s scope, so the run is checked under
// the checked AOT oracle (ASan/LSan): a double-free or leak of the borrowed Item fails
// the run. The class-conformer variant of the same witness (`class Item:
// IPick<Item>` with `public static method Pick(a: Item, b: Item): Item => a;`)
// is NOT covered here: the class-conformer analogue is rejected today; see the
// repro comment on #5818.

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// §28.3 note 1 example: a record conformer holding a borrow of its Item.
const char* kRecordPairPick =
    "module M; "
    "public interface IPick<T> where T: IPick<T> "
    "  static method Pick(a: T, b: T): T; "
    "end IPick; "
    "public class Item "
    "  public Value: int; "
    "  public constructor(v: int) Value := v; end constructor; "
    "end Item; "
    "public record Pair(It: Item): IPick<Pair> "
    "  public static method Pick(a: Pair, b: Pair): Pair => a; "
    "end Pair; "
    "public class C "
    "  public static method Main(): int "
    "    using var x := new Item(7); "
    "    var a := new Pair(x); "
    "    var b := new Pair(x); "
    "    var r := Pair.Pick(a, b); "
    "    return r.It.Value; "
    "  end Main; "
    "end C; ";

}  // namespace

BEZEL_TEST(ownership_static_interface_witness_5818,
           record_conformer_return_borrowed_param_spec_28_3_note1) {
    JitResult r = runAotChecked(kRecordPairPick);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
