//! case: s28_9_0903
//! doc: spec
//! section: 28.9
//! title: a chain call on an extension method result that returns the receiver's field closure
//! rule: **Function-typed values** — values of any function type `(T1, ..., Tn) -> R`, regardless
//!   of whether they capture.
//! cites: 27.5, 26.1
//! expect: run
//! output: 8
//! issue: 8141
module Conf;

public class Holder
  public Fn: (int) -> int;
  public Fn2: (int) -> int;

  public constructor()
    this.Fn := lambda(x: int) => x + 7;
    this.Fn2 := lambda(x: int) => x + 8;
  end constructor;

end Holder;

public extension Holder
  public method XGet(): (int) -> int
    return this.Fn;
  end XGet;
end extension;

public class Program
  public static method Main()
    using var h := new Holder();
    Console.WriteLine(h.XGet()(1));
  end Main;
end Program;
