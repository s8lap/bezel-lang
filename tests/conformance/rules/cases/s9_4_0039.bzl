//! case: s9_4_0039
//! doc: spec
//! section: 9.4
//! title: an increment through a two-hop chain of record-typed fields is rejected
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

public record Mid
  public I: Inner;

  public constructor(i: Inner)
    I := i;
  end constructor;
end Mid;

public record Outer
  public J: Mid;

  public constructor(i: Inner)
    J := new Mid(i);
  end constructor;

  public method M()
    this.J.I.X++;
  end M;
end Outer;

public class Program
  public static method Main()
    var o := new Outer(new Inner(1));
    o.M();
    Console.WriteLine(o.J.I.X);
  end Main;
end Program;
