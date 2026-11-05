//! case: s27_3_2017
//! doc: spec
//! section: 27.3
//! title: lambda arguments returning Box<int> infer or bind U through an
//!   owned Box<U> slot
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1, 14.5
//! expect: run
//! output: 4
//!   6
module Conf;

class Box<T>
  private _v: int;

  public constructor(v: int)
    _v := v;
  end constructor;

  public method V(): int => _v;
end Box;

public class Program
  public static method Make<U>(f: () -> owned Box<U>): owned Box<U> => f();

  public static method Main()
    using var b := Make<int>(lambda() => new Box<int>(4));
    Console.WriteLine(b.V());
    using var c := Make(lambda() => new Box<int>(6));
    Console.WriteLine(c.V());
  end Main;
end Program;
