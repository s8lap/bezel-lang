//! case: s22_6_7362
//! doc: spec
//! section: 22.6
//! title: a lambda returning an int selects the `() -> int` constructor over
//!   `() -> int?`
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1
//! expect: run
//! stdout: B:41
//! notes: Pins the #6642 fix: `int` equals the slot at `() -> int` and
//!   converts to the nullable slot at `() -> int?`.
module Conf;

public class K
  public constructor(f: () -> int?) Console.WriteLine("A"); end;
  public constructor(f: () -> int) Console.WriteLine("B:" + f().ToString()); end;
end;
public static class Program
  public static method Main(): int
    var n: int := 41;
    var k := new K(lambda() => n);
    return 0;
  end;
end;
