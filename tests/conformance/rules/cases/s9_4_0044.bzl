//! case: s9_4_0044
//! doc: spec
//! section: 9.4
//! title: a store and increment into a local copy of a record-typed field is not a store into the receiver
//! rule: The receiver's own storage is what an lvalue chain rooted at `this` reaches without leaving the
//!   value
//! cites: 9.5
//! expect: run
//! output: 9
//! issue: 8100
module Conf;

public record Inner
  public X: int;

  public constructor(x: int)
    X := x;
  end constructor;
end Inner;

public record Outer
  public F: Inner;

  public constructor(f: Inner)
    F := f;
  end constructor;

  public method M(): int
    var c := F;
    c.X := 7;
    c.X++;
    return c.X + F.X;
  end M;
end Outer;

public class Program
  public static method Main()
    var o := new Outer(new Inner(1));
    Console.WriteLine(o.M());
  end Main;
end Program;
