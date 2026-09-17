//! case: s15_3_0011
//! doc: spec
//! section: 15.3
//! title: owned on a move-only value parameter is admitted as a redundant marker
//! rule: A parameter declared `(owned x: V)` is identical to plain `(x: V)` for
//!   a move-only V; the modifier is admitted for documentation but is redundant
//!   (§15.3; §28.1's type requirement, clause 1).
//! cites: 15.3, 28.1
//! expect: run
//! stdout: x
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Sink
  public static method Take(owned a: [1 owned]Widget)
    Console.WriteLine(a[0].Tag);
  end Take;
end Sink;

public class Program
  public static method Main()
    Sink.Take([new Widget("x")]);
  end Main;
end Program;
