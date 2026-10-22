//! case: s14_1_0005
//! doc: spec
//! section: 14.1
//! title: a generic class method and a virtual method return a generic choice carrying their payload
//! rule: "For a **generic** choice the type arguments are written at the construction (`Result<int, string>.Ok(value)`)" (§35.3, v0.340); a generic member is evaluated at each instantiation after substituting the type arguments (§3.6 rule 2, §14.1), so the payload constructed in the generic body is the payload matched by the caller. `Box<int>.Get(7)` and the virtual `VGet(8)` return `Opt<int>.Some(x)`, and the caller's match reads 7 and 8.
//! cites: 35.3, 3.6
//! expect: run
//! output: 7
//!   8
module Conf;

public choice Opt<T>
  Some(v: T),
  No;
end Opt;

public class Box<T>
  public method Get(x: T): Opt<T> => Opt<T>.Some(x);
  public virtual method VGet(x: T): Opt<T> => Opt<T>.Some(x);
end Box;

public class Program
  public static method Main()
    var b := new Box<int>();
    Console.WriteLine((case b.Get(7) of Some(var v) => $"{v}", No => "no"));
    Console.WriteLine((case b.VGet(8) of Some(var v) => $"{v}", No => "no"));
  end Main;
end Program;
