//! case: s15_3_0012
//! doc: spec
//! section: 15.3
//! title: equality over move-only values borrows each slot and leaves both operands readable
//! rule: Move-only values support equality via borrowing comparison (reads each
//!   slot without moving), provided their element types support equality
//!   (§15.3; operator operands bind borrows, §25.2).
//! cites: 15.3, 15.1.2
//! expect: run
//! output: False
//!   x
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Program
  public static method Main()
    var a: [1 owned]Widget := [new Widget("x")];
    var b: [1 owned]Widget := [new Widget("x")];
    Console.WriteLine(a = b);
    Console.WriteLine(a[0].Tag);
  end Main;
end Program;
