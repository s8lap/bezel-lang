//! case: s9_4_0037
//! doc: spec
//! section: 9.4
//! title: a this-qualified decrement through a record-typed field chain is rejected
//! rule: a compound assignment or `++`/`--` classifies through its
//!   [§6.6](#sec-compound-assignments)/[§17.6](#sec-the-increment-and-decrement-operators) desugaring
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
    this.F.X--;
  end M;
end Outer;

public class Program
  public static method Main()
    var o := new Outer(new Inner(1));
    o.M();
    Console.WriteLine(o.F.X);
  end Main;
end Program;
