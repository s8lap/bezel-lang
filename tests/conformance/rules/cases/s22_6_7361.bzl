//! case: s22_6_7361
//! doc: spec
//! section: 22.6
//! title: a lambda returning an int selects the exact `() -> int` constructor
//!   over `() -> long`
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1
//! expect: run
//! stdout: B:42
//! notes: Pins the #6642 fix: the inferred return type `int` is exact at the
//!   `() -> int` slot and converting at `() -> long`; the exact candidate is
//!   better at the only lambda position.
module Conf;

public class K
  public constructor(f: () -> long) Console.WriteLine("A"); end;
  public constructor(f: () -> int) Console.WriteLine("B:" + f().ToString()); end;
end;
public static class Program
  public static method Main(): int
    var n: int := 41;
    var k := new K(lambda() => n + 1);
    return 0;
  end;
end;
