//! case: s15_3_0007
//! doc: spec
//! section: 15.3
//! title: reassigning a move-only local is frozen while a borrow of a slot's object is live
//! rule: The reassignment is a consume of the displaced value — at a local,
//!   frozen while a live local borrow of the displaced value exists, a borrow
//!   of any slot's object included, since the pass disposes it (§15.3).
//! cites: 15.3, 28.7
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Program
  public static method Run()
    var a: [1 owned]Widget := [new Widget("first")];
    var s := a[0];
    a := [new Widget("second")];
    Console.WriteLine(s.Tag);
  end Run;
end Program;
