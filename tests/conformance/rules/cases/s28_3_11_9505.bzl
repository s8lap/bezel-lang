//! case: s28_3_11_9505
//! doc: spec
//! section: 28.3.11
//! title: a bare iterator call over a local is rejected whatever the element type of the frame
//! rule: The returned deferred-activation frame carries its arguments' borrows: it is treated as a
//!   borrow-carrying value whose sources are the call's receiver and the sources its plain arguments
//!   contribute
//! cites: 28.3, 29.4
//! expect: reject
//! diagnostic: outlive
//! notes: Pins the #9155 fix-forward (pick 8e9093ab1 / 6866827ba): a bare method name called directly is
//!   not a function-value call, so the deferred-activation arm of the borrow-escape check judges the
//!   call. Before it the program was admitted. Accepting twin: s28_3_11_9506. The frame carries the
//!   Item argument's borrow though its elements are ints (the classification is by declared return
//!   type, not element type).
module Conf;

public class Item
  public constructor()
  end constructor;
  destructor
  end destructor;
end Item;

public class C
  public static method Nums(x: Item): IEnumerable<int>
    yield return 1;
  end Nums;

  public static method F(): IEnumerable<int>
    var t := new Item();
    return Nums(t);
  end F;
end C;
