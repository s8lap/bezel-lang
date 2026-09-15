//! case: s7_1_0002
//! doc: spec
//! section: 7.1
//! title: a nullable reference does not implicitly convert to its non-nullable form
//! rule: The reverse (nullable to non-nullable) requires a null check or `!`.
//! expect: reject
//! diagnostic: null check
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var maybe: Widget? := new Widget();
    var w: Widget := maybe;
  end Main;
end Program;
