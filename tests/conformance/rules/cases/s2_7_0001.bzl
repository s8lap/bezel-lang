//! case: s2_7_0001
//! doc: spec
//! section: 2.7
//! title: a chain head resolves to a reachable type over a same-named member
//! rule: Chain head — name followed by `.`: enclosing-type members rank
//!   last, after aliases — a reachable type outranks a same-named member.
//!   No warning fires because the member's declared type is the resolved
//!   type (the `property Color: Color` idiom).
//! cites: 2.7
//! expect: run
//! stdout: R
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

  public method ResetToRed()
    Color := Color.Red;   // RHS chain head `Color.` resolves to the enum
                           // TYPE, not this method's own `Color` property
  end ResetToRed;
end Widget;

public class Program
  public static method Main()
    using var w := new Widget(Color.Blue);
    w.ResetToRed();
    Console.WriteLine(w.Color.Name());
  end Main;
end Program;
