//! case: s2_7_0002
//! doc: spec
//! section: 2.7
//! title: a bare value read resolves to a same-named member over a type
//! rule: Bare value read: enclosing-type members rank between local scope
//!   and module declarations — a member outranks a same-named top-level
//!   type.
//! cites: 2.7
//! expect: run
//! stdout: G
module Conf;

public enum Color
  Red, Green, Blue;
  public method Name(): string =>
    (case this of Red => "R", Green => "G", Blue => "B");
end Color;

public class Widget
  public property Color: Color;

  public constructor(c: Color)
    Color := c;
  end constructor;

  public method Describe(): string
    var c := Color;   // bare value read: the property (member) wins over
                       // the enclosing module's `Color` type
    return c.Name();
  end Describe;
end Widget;

public class Program
  public static method Main()
    using var w := new Widget(Color.Green);
    Console.WriteLine(w.Describe());
  end Main;
end Program;
