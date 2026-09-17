//! case: s28_3_0025
//! doc: spec
//! section: 28.3
//! title: a borrow assigned to a local whose scope strictly encloses its source is rejected at the assignment
//! rule: A borrow may not be assigned to a local whose scope strictly encloses its
//!   source's scope; rejected at the assignment, not at the later use.
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo(flag: bool)
    var outer: Widget;
    if flag then
      using var inner := new Widget();
      outer := inner;
    end;
  end Demo;
end Program;
