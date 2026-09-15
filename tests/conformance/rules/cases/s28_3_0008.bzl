//! case: s28_3_0008
//! doc: spec
//! section: 28.3
//! title: an if-expression merging an owned arm with an unmarked owned-binding arm is rejected
//! rule: owned meets borrowed at an if-expression merge is rejected; an unmarked read
//!   of a named owned binding is a borrow arm.
//! expect: reject
//! diagnostic: borrowed
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo(useFresh: bool)
    var cached := new Widget();
    var picked := (if useFresh then new Widget() else cached);
    picked.Dispose();
  end Demo;
end Program;
