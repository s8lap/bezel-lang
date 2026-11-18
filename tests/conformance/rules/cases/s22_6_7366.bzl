//! case: s22_6_7366
//! doc: spec
//! section: 22.6
//! title: a lambda returning a string selects the `() -> string` constructor
//!   over `() -> Object`
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1
//! expect: run
//! stdout: B:hi
//! notes: Pins the #6642 fix: `string` equals the slot at `() -> string` and
//!   converts to Object at the other slot.
module Conf;

public class K
  public constructor(f: () -> Object) Console.WriteLine("A"); end;
  public constructor(f: () -> string) Console.WriteLine("B:" + f()); end;
end;
public static class Program
  public static method Main(): int
    var k := new K(lambda() => "hi");
    return 0;
  end;
end;
