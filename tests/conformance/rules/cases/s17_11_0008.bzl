//! case: s17_11_0008
//! doc: spec
//! section: 17.11
//! title: default of a non-nullable reference type is rejected
//! rule: "`default(T)` is a compile error for the non-defaultable types: ...
//!   A non-nullable reference type."
//! cites: 17.11
//! expect: reject
//! diagnostic: defaultable
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var w := default(Widget);
  end Main;
end Program;
