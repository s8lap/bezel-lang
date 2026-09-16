//! case: s15_3_0012
//! doc: spec
//! section: 15.3
//! title: equality over move-only values compares slot-wise by borrow and leaves both operands readable
//! rule: Move-only values support equality via borrowing comparison (reads each
//!   slot without moving), provided their element types support equality
//!   (§15.3; operator operands bind borrows, §25.2). An owning inline array's
//!   equality is element-by-element (§3.6's table), so two arrays whose slots
//!   compare equal are equal and two whose slots differ are not.
//! cites: 15.3, 15.1.2, 3.6, 17.11
//! expect: run
//! output: False
//!   True
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
    var c: [1 owned]Widget? := default([1 owned]Widget?);
    var d: [1 owned]Widget? := default([1 owned]Widget?);
    Console.WriteLine(a = b);
    Console.WriteLine(c = d);
    Console.WriteLine(a[0].Tag);
  end Main;
end Program;
