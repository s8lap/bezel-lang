//! case: s15_3_0010
//! doc: spec
//! section: 15.3
//! title: a move-only argument is moved-from after the call and may not be read
//! rule: A parameter declared `(x: V)` for a move-only V is by value-move
//!   (transfer ownership); the caller's argument expression is in moved-from
//!   state after the call (§15.3).
//! cites: 15.3
//! expect: reject
//! diagnostic: moved-from
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Sink
  public static method Take(a: [1 owned]Widget)
    Console.WriteLine(a[0].Tag);
  end Take;
end Sink;

public class Program
  public static method Run()
    var a: [1 owned]Widget := [new Widget("x")];
    Sink.Take(a);
    Console.WriteLine(a[0].Tag);
  end Run;
end Program;
