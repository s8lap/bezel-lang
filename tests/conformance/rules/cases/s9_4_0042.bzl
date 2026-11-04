//! case: s9_4_0042
//! doc: spec
//! section: 9.4
//! title: stores and increments past a class-typed hop and a record-typed field of that class are legal
//! rule: a chain that hops through a class- or interface-typed member leaves the receiver
//! cites: 9.7
//! expect: run
//! output: 6
//!   3
//! issue: 8100
module Conf;

public record Inner
  public X: int;

  public constructor(x: int)
    X := x;
  end constructor;
end Inner;

public class Cell
  public Value: int;
  public In: Inner;

  public constructor(value: int)
    Value := value;
    In := new Inner(0);
  end constructor;
end Cell;

public record Outer
  public H: Cell;

  public constructor(h: Cell)
    H := h;
  end constructor;

  public method Set(v: int)
    H.In.X := v;
  end Set;

  public method Bump()
    H.In.X++;
    H.Value += 2;
  end Bump;
end Outer;

public class Program
  public static method Main()
    using var c := new Cell(1);
    var o := new Outer(c);
    o.Set(5);
    o.Bump();
    Console.WriteLine(c.In.X);
    Console.WriteLine(c.Value);
  end Main;
end Program;
