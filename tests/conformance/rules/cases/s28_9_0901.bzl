//! case: s28_9_0901
//! doc: spec
//! section: 28.9
//! title: a chain call on a virtual getter result whose override returns a field closure
//! rule: **Function-typed values** — values of any function type `(T1, ..., Tn) -> R`, regardless
//!   of whether they capture.
//! cites: 27.5, 12.1
//! expect: run
//! output: 8
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

  public virtual method Get(): (int) -> int
    return lambda(x: int) => x + 3;
  end Get;
end Holder;

public class Sub: Holder
  public constructor()
    base();
  end constructor;

  public override method Get(): (int) -> int
    return this.Fn;
  end Get;
end Sub;

public class Program
  public static method Main()
    using var h: Holder := new Sub();
    using var b := new Holder();
    Console.WriteLine(h.Get()(1));
    Console.WriteLine(b.Get()(1));
  end Main;
end Program;
