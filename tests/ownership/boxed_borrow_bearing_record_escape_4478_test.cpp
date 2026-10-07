// fix #4478 (§8.6, v0.315): a boxed borrow-bearing record escaping its source scope
// is not rejected.
//
// §8.6 verbatim: "Storing a boxed borrow-bearing record in a field, returning it
// from a method, or capturing it in a closure that escapes the source scope is
// rejected by the same rules as the unboxed form."
//
// The specific escape shape this issue covers: a borrow-bearing record
// (`BorrowRec(W: Widget)`) is constructed and immediately stored in an inline
// array of interface type (`[1]IHolder`). The interface slot causes the record
// to be boxed (§8.6: "When a value type is used where Object or an interface
// is expected, the value is boxed"). The box is a field of `Holder`, so it
// outlives the source scope of the borrow (`w` in `Make()`). Reading through
// the box's interface dispatches on the dead borrow, giving a
// heap-use-after-free.
//
// The inline array's non-owning slot stores a BORROW of the record, not the
// record itself — but a boxed borrow-bearing record carries its borrow into the
// box, and that borrow's source dies at scope end while the field (and thus the
// box) persists. This is exactly the unboxed form's §9.7 field-store rule's
// premise: "a borrow-bearing record value may not be stored in an instance field
// of a class at all". Boxing does not lift the constraint.
//
// ROOT CAUSE: checkBorrowStoreEscape's §9.7 leg reads the RHS expression's
// type, not the types of the elements inside a collection literal. The literal
// `[new BorrowRec(w)]` has type `[1]IHolder` (inline array), which
// isBorrowBearingRecord never classifies. The element `new BorrowRec(w)` is a
// borrow-bearing record, but its borrow is invisible through the boxing layer
// the inline array's interface element type introduces.
//
// THE FIX: when a collection literal is the RHS of a field store, check each
// element that is a NewExpr constructing a borrow-bearing record. If the element
// is stored to a class/interface-typed slot (i.e., the element type of the
// target inline array is Named with a Class or Interface declaration), the
// element is boxed, and the §8.6 / §9.7 constraint applies.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// Minimal widget + borrow-bearing record setup
std::string makeBorrowRecModule() {
    return R"(
module M;
interface IHolder method GetV(): string; end IHolder;
class Widget: IHolder
  private _s: string;
  public constructor(s: string) _s := s; end constructor;
  public method GetV(): string return _s; end GetV;
end Widget;
record BorrowRec(W: Widget): IHolder
  public method GetV(): string => W.GetV();
end BorrowRec;
)";
}

// Control: borrow-bearing record at a value-typed (non-boxing) slot — accepted.
// A borrow-bearing record stored to a slot that does NOT box it (value-typed
// element or non-interface class element) does not trigger the §8.6 constraint.
BEZEL_TEST(boxed_borrow_record_escape_4478,
           borrow_bearing_record_at_value_slot_accepted) {
    auto src = makeBorrowRecModule() + R"(
class Holder
  public Items: [1]BorrowRec;  // value-typed slot, no boxing
  public constructor(w: Widget) Items := [new BorrowRec(w)]; end constructor;
end Holder;
class C
  public static method Main(): int return 0; end Main;
end C;
)";
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The defect: borrow-bearing record boxed into interface slot and stored in field
BEZEL_TEST(boxed_borrow_record_escape_4478,
           boxed_borrow_bearing_record_in_interface_slot_field_rejected) {
    auto src = makeBorrowRecModule() + R"(
class Holder
  public Items: [1]IHolder;
  public constructor(w: Widget) Items := [new BorrowRec(w)]; end constructor;
end Holder;
class C
  public static method Make(): owned Holder
    using var w := new Widget("hello");
    return new Holder(w);
  end Make;
  public static method Main(): int
    using var h := Make();
    return h.Items[0].GetV().Length;
  end Main;
end C;
)";
    auto r = parseAndCheckOwnership(src);
    // Should be rejected: the boxed borrow-bearing record escapes the source scope
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "borrow-bearing record may not be stored in an instance field"));
}

} // namespace
