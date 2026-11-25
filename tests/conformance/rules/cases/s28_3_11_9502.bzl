//! case: s28_3_11_9502
//! doc: spec
//! section: 28.3.11
//! title: a bare async call over a scope-bound local may not be returned as the task frame
//! rule: A returned frame carrying a borrow of anything else — a scope-bound local, a `using var`, a
//!   temporary — is rejected at the return site, since the frame would outlive the source
//! cites: 28.3, 30.1
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
  public static async method Use(x: Item): Task<int>
    return 1;
  end Use;

  public static method F(): Task<int>
    var t := new Item();
    return Use(t);
  end F;
end C;
