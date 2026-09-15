//! case: s17_11_0013
//! doc: spec
//! section: 17.11
//! title: default of an inline array of a non-defaultable element type is rejected
//! rule: "An inline array whose element type is not defaultable.
//!   `default([3]Connection)` (non-nullable Connection elements) is a
//!   compile error; the user must supply explicit initializers."
//! cites: 17.11
//! expect: reject
//! diagnostic: defaultable
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Program
  public static method Main()
    var a := default([3]Connection);
  end Main;
end Program;
