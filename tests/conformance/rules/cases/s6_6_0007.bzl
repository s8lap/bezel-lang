//! case: s6_6_0007
//! doc: spec
//! section: 6.6
//! title: the right operand of ??= is evaluated only when the assignment occurs
//! rule: ??= is not a read-modify-write: x ??= e assigns e only when x's
//!   current value is null. The LHS chain is evaluated once, the getter once;
//!   e is evaluated — and the setter invoked — only when the assignment
//!   actually occurs.
//! cites: 6.6, 7.7
//! expect: run
//! output: 0
//!   1
module Conf;

public class Source
  private _calls: int := 0;

  public constructor()
  end constructor;

  public property Calls: int => _calls;

  public method Five(): int
    _calls := _calls + 1;
    return 5;
  end Five;
end Source;

public class Program
  public static method Main()
    using var src := new Source();
    var present: int? := 7;
    present ??= src.Five();
    Console.WriteLine(src.Calls);
    var absent: int? := null;
    absent ??= src.Five();
    Console.WriteLine(src.Calls);
  end Main;
end Program;
