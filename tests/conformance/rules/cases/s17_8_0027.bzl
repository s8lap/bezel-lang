//! case: s17_8_0027
//! doc: spec
//! section: 17.8
//! title: a null-forgiving read through a payload binding of a borrowing match only reads and consumes nothing
//! rule: it does not move, copy, or borrow beyond what reading `x` already does.
//! cites: 19.3, 35.5
//! expect: run
//! output: 7
//!   2
//! issue: 7892
module Conf;
public class Widget
  public static _n: int := 0;
  public constructor() end constructor;
  destructor Widget._n := Widget._n + 1; end destructor;
  public method Ping(): int => 7;
end Widget;
public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer;
public class Program
  static method Body()
    var o := Outer.W(new Widget(), new Widget());
    case o of
      W(var a, _) => do Console.WriteLine(a!.Ping()); end,
      X => ()
    end case;
  end Body;

  public static method Main()
    Body();
    Console.WriteLine(Widget._n);
  end Main;
end Program;
