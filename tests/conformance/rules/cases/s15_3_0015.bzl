//! case: s15_3_0015
//! doc: spec
//! section: 15.3
//! title: a whole-value move out of a move-only local creates no restoration obligation
//! rule: A whole-value move — a plain binding, a plain argument, a return —
//!   creates no restoration obligation on a local: it leaves the local holding
//!   no value rather than a hole, so no pass walks it (§15.3).
//! cites: 15.3
//! expect: run
//! output: first
//!   done
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
  public static method Main()
    var a: [1 owned]Widget := [new Widget("first")];
    Sink.Take(a);
    Console.WriteLine("done");
  end Main;
end Program;
