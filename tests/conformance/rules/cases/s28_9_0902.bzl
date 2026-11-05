//! case: s28_9_0902
//! doc: spec
//! section: 28.9
//! title: a chain call on a method result chosen by a case statement over field closures and a lambda
//! rule: **Function-typed values** — values of any function type `(T1, ..., Tn) -> R`, regardless
//!   of whether they capture.
//! cites: 27.5, 17.5
//! expect: run
//! output: 8
//!   9
//!   4
//! issue: 8141
module Conf;

public class Holder
  public Fn: (int) -> int;
  public Fn2: (int) -> int;

  public constructor()
    this.Fn := lambda(x: int) => x + 7;
    this.Fn2 := lambda(x: int) => x + 8;
  end constructor;

  public method GetC(k: int): (int) -> int
    case k of
      0 => do return this.Fn; end,
      1 => do return this.Fn2; end,
      _ => do return lambda(x: int) => x + 3; end
    end case;
    return this.Fn;
  end GetC;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    Console.WriteLine(h.GetC(0)(1));
    Console.WriteLine(h.GetC(1)(1));
    Console.WriteLine(h.GetC(2)(1));
  end Main;
end Program;
