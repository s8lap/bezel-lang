//! case: s28_10_0010
//! doc: spec
//! section: 28.10
//! title: an unmarked using var over a move-only local adopts rather than pins
//! rule: Where `x` is a local of **move-only value type**, the unmarked
//!   `using var y := x;` **adopts**: a move-only binding has no borrow reading
//!   to disambiguate, so the plain read moves, `y` carries the value's owned
//!   slots to the end of *its* block, and `x` is left moved-from.
//! cites: 28.10, 15.3, 28.3
//! expect: run
//! output: inner
//!   widget disposed
//!   outer
//! issue: 4376
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    var a: [1 owned]Widget := [new Widget()];
    do
      using var b := a;
      Console.WriteLine("inner");
    end;
    Console.WriteLine("outer");
  end Main;
end Program;
