//! case: s9_4_0035
//! doc: spec
//! section: 9.4
//! title: a store through a record-typed field chain into the receiver's own storage is rejected
//! rule: The receiver's own storage is what an lvalue chain rooted at `this` reaches without leaving the
//!   value
//! cites: 9.2, 17.6
//! expect: reject
//! diagnostic: receiver's own storage
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

  public method M()
    F.X := 5;
  end M;
end Outer;

public class Program
  public static method Main()
    var o := new Outer(new Inner(1));
    o.M();
    Console.WriteLine(o.F.X);
  end Main;
end Program;
