//! case: s28_9_0006
//! doc: spec
//! section: 28.9
//! title: move through a closure's captured borrow of a move-only local is rejected
//! rule: Capturing a move-only local follows the same rule — the closure
//!   captures a borrow whose source is the local's enclosing scope; reads of
//!   slots through the captured borrow are admitted, `move arr[i]` through
//!   it is not, because the closure does not own the array.
//! cites: 28.9, 15.3
//! expect: reject
//! diagnostic: does not own
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Run()
    var arr: [1 owned]Widget := [new Widget()];
    var taker := lambda() => do
      var w := move arr[0];
      w.Dispose();
    end;
    taker();
  end Run;
end Program;
