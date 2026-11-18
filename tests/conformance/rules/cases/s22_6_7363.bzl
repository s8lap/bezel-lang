//! case: s22_6_7363
//! doc: spec
//! section: 22.6
//! title: a lambda written with `: int` selects the exact `() -> int`
//!   constructor over `() -> long`
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 27.3
//! expect: run
//! stdout: B:41
//! notes: Pins the #6642 fix for the ascribed spelling `lambda(): int => n`,
//!   the admitting side of s27_3_7364.
module Conf;

public class K
  public constructor(f: () -> long) Console.WriteLine("A"); end;
  public constructor(f: () -> int) Console.WriteLine("B:" + f().ToString()); end;
end;
public static class Program
  public static method Main(): int
    var n: int := 41;
    var k := new K(lambda(): int => n);
    return 0;
  end;
end;
