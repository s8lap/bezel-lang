//! case: s28_3_0020
//! doc: spec
//! section: 28.3
//! title: move on a borrow is rejected
//! rule: `move` is also an error on a borrow: the marker asserts a transfer, and a
//!   plain parameter carries no obligation to transfer.
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Sink
  public method Peek(w: Widget)
    var b := move w;
  end Peek;
end Sink;
