//! case: s28_3_0009
//! doc: spec
//! section: 28.3
//! title: marking the if-expression arm transfer equalizes the merge
//! rule: The marked arm transfer `move x` as an if-expression branch is an owned
//!   producer, equalizing a merge with another owned arm.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo(useFresh: bool)
    var cached := new Widget();
    var picked := (if useFresh then new Widget() else move cached);
    picked.Dispose();
  end Demo;
end Program;
