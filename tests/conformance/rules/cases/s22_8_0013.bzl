//! case: s22_8_0013
//! doc: spec
//! section: 22.8
//! title: a lambda in a constructor body may read a readonly field the constructor assigned
//! rule: so an assignment to a `readonly` field inside a lambda ... is a
//!   compile error wherever the lambda stands ... assign the field in the
//!   constructor and let the lambda read it.
//! cites: 22.8, 27.3
//! expect: accept
module Conf;

public class Gauge
  private readonly _level: int;

  public constructor()
    _level := 5;
    var readLevel := lambda() => _level;
    Console.WriteLine(readLevel());
  end constructor;

  public method Level(): int => _level;
end Gauge;
