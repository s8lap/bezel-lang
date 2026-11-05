//! case: s27_3_2008
//! doc: spec
//! section: 27.3
//! title: a lambda returning Box<int> at a Box<int> slot is admitted
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1
//! expect: run
//! stdout: 9
module Conf;

class Box<T>
  private _v: int;

  public constructor(v: int)
    _v := v;
  end constructor;

  public method V(): int => _v;
end Box;

public class Program
  public static method Main()
    using var b := new Box<int>(9);
    var f: (Box<int>) -> Box<int> := lambda(x) => x;
    Console.WriteLine(f(b).V());
  end Main;
end Program;
