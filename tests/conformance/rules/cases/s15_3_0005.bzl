//! case: s15_3_0005
//! doc: spec
//! section: 15.3
//! title: a wholly moved-from move-only local is readable again after reassignment
//! rule: After the move, `x` is in moved-from state and may not be read until
//!   reassigned; the standard restoration mechanism is to assign a fresh owned
//!   value (§15.3).
//! cites: 15.3
//! expect: run
//! output: second
//!   first
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Program
  public static method Main()
    var a: [1 owned]Widget := [new Widget("first")];
    var b := a;
    a := [new Widget("second")];
    Console.WriteLine(a[0].Tag);
    Console.WriteLine(b[0].Tag);
  end Main;
end Program;
