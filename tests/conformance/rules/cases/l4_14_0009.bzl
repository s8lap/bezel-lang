//! case: l4_14_0009
//! doc: stdlib
//! section: 4.14
//! title: the natural restoration for a moved-from nullable slot is assigning null
//! rule: move buf[i] is admitted and produces an owned T? value, leaving the
//!   slot in moved-from state; the natural restoration for a nullable slot
//!   is buf[i] := null.
//! cites: 4.14, 28.8
//! expect: run
//! stdout: moved
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<Widget>(1);
    buf[0] := new Widget();
    using var w := move buf[0];
    buf[0] := null;
    Console.WriteLine("moved");
  end Main;
end Program;
