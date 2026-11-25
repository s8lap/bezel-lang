//! case: s28_3_11_9501
//! doc: spec
//! section: 28.3.11
//! title: a bare iterator call over a scope-bound local may not be returned as the frame
//! rule: A returned frame carrying a borrow of anything else — a scope-bound local, a `using var`, a
//!   temporary — is rejected at the return site, since the frame would outlive the source
//! cites: 28.3, 29.4
//! expect: reject
//! diagnostic: outlive
//! notes: Pins the #9155 fix-forward (pick 8e9093ab1 / 6866827ba): a bare method name called directly is
//!   not a function-value call, so the deferred-activation arm of the borrow-escape check judges the
//!   call. Before it the program was admitted. Accepting twin: s28_3_11_9506.
module Conf;

public class Item
  public constructor()
  end constructor;
  destructor
  end destructor;
end Item;

public class C
  public static method OnceItem(x: Item): IEnumerable<Item>
    yield return x;
  end OnceItem;

  public static method F(): IEnumerable<Item>
    var t := new Item();
    return OnceItem(t);
  end F;
end C;
