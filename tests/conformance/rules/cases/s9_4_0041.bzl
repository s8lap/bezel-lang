//! case: s9_4_0041
//! doc: spec
//! section: 9.4
//! title: a record-typed field chain passed as a ref argument is the same store by other syntax
//! rule: a `this`-rooted chain reaching the receiver's own storage passed as a `ref`/`out` argument
//!   ([§22.3](#sec-parameter-modifiers))
//! cites: 9.2, 22.3
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
    Program.Inc(ref F.X);
  end M;
end Outer;

public class Program
  public static method Inc(ref v: int)
    v := v + 1;
  end Inc;

  public static method Main()
    var o := new Outer(new Inner(1));
    o.M();
    Console.WriteLine(o.F.X);
  end Main;
end Program;
