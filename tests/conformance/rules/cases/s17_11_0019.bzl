//! case: s17_11_0019
//! doc: spec
//! section: 17.11
//! title: default of an inline array with a non-defaultable element type is rejected
//! rule: "An inline array whose element type is not defaultable.
//!   `default([3]Connection)` (non-nullable Connection elements) is a compile
//!   error; the user must supply explicit initializers."
//! cites: 17.11, 15.1
//! expect: reject
//! diagnostic: defaultable
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var slots: [3]Widget := default([3]Widget);
  end Main;
end Program;
