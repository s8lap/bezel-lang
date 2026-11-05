//! case: s27_3_2015
//! doc: spec
//! section: 27.3
//! title: a lambda argument returning Box<string> at the
//!   receiver-substituted Box<string> slot is admitted
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1, 14.5
//! expect: run
//! stdout: 5
module Conf;

class Box<T>
  private _v: int;

  public constructor(v: int)
    _v := v;
  end constructor;

  public method V(): int => _v;
end Box;

class Wrap<T>
  public method Map(v: int, f: (int) -> owned Box<T>): int
    using var b := f(v);
    return b.V();
  end Map;
end Wrap;

public class Program
  public static method Main()
    using var w := new Wrap<string>();
    Console.WriteLine(w.Map(5, lambda(x) => new Box<string>(x)));
  end Main;
end Program;
