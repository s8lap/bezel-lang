//! case: s28_3_0014
//! doc: spec
//! section: 28.3
//! title: move on a discard right-hand side is rejected as redundant
//! rule: The transfer marker is rejected as redundant on a discard right-hand side: `_
//!   := x;`, never `_ := move x;`.
//! expect: reject
//! diagnostic: discard
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    _ := move w;
  end Demo;
end Program;
