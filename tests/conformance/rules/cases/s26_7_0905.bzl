//! case: s26_7_0905
//! doc: spec
//! section: 26.7
//! title: an extension body may not store the receiver through a const alias with an explicit type into a
//! rule: The receiver is always a borrow; extensions cannot store `this`, return it as `owned`, or
//!   dispose it.
//! cites: 26.7
//! expect: reject
//! diagnostic: store
module Conf;

public class Holder
  public o: Object?;
  public b: Box?;
  public k: int;

  public constructor()
  end constructor;
end Holder;

public class Box
  public n: int;

  public constructor()
  end constructor;
end Box;

public class Util
  public static method Use(b: Box): int => b.n;
end Util;

public extension Box
  public method Stash(h: Holder)
    const a: Box := this;
    h.b := a;
  end Stash;
end extension;

public class Program
  public static method Main()
    using var h := new Holder();
    using var b := new Box();
    b.Stash(h);
    Console.WriteLine("ran");
  end Main;
end Program;
