//! case: s22_3_0056
//! doc: spec
//! section: 22.3
//! title: the move call-site marker is rejected at a function-value invoke
//! rule: Writing [the move marker] ... on a non-`owned` parameter position is
//!   likewise an error (at a non-`owned` position the diagnostic names the borrow
//!   default — an owned expression may not feed a borrow slot) (§22.3, v0.343).
//!   The marker is a property of the argument position, not of the spelling used
//!   to make the call — so a function-value invoke rejects it as the direct call
//!   does.
//! cites: 22.3, 27.7
//! expect: reject
//! diagnostic: only valid on an argument to an `owned` parameter
//! issue: 7124
module Conf;

public class Item
  public constructor() end constructor;
end Item;

public class Sink
  public method Accept(it: Item): int return 0; end Accept;
end Sink;

public class Program
  public static method Main(): int
    var s := new Sink();
    var d := new Item();
    var f: (Item) -> int := s.Accept;
    _ := f(move d);
    return 0;
  end Main;
end Program;
