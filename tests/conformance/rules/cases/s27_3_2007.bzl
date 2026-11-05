//! case: s27_3_2007
//! doc: spec
//! section: 27.3
//! title: a lambda returning Box<string> at a Box<int> slot is rejected
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1, 12.9
//! expect: reject
//! diagnostic: implicitly convertible
//! notes: Failed on the compiler before the #8168 fix and passes on the
//!   staging tip.
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
    var f: (Box<string>) -> Box<int> := lambda(x) => x;
  end Main;
end Program;
