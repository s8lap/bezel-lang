//! case: l4_14_0009
//! doc: stdlib
//! section: 4.14
//! title: a bare move buf[i] on a class-typed buffer slot stores null in the same step
//! rule: `move buf[i]` is admitted and produces an owned `T?` value, storing
//!   `null` into the slot in the same step; `move buf[i] with e` stores `e`
//!   instead (§28.8.1).
//! cites: 4.14, 28.8.1
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
