//! case: s27_3_7364
//! doc: spec
//! section: 27.3
//! title: a lambda written with `: int` has no applicable `() -> long` or `()
//!   -> double` constructor
//! rule: so an ascription narrows what the position admits
//! cites: 22.6
//! expect: reject
//! diagnostic: no overload
//! notes: Pins the #6642 fix: the ascription narrows what the position
//!   admits, so a lambda ascribed `: int` does not fit a `() -> long` or `()
//!   -> double` slot. The accepting twin adds an `() -> int` constructor
//!   (s22_6_7363).
module Conf;

public class K
  public constructor(f: () -> long) Console.WriteLine("A"); end;
  public constructor(f: () -> double) Console.WriteLine("B"); end;
end;
public static class Program
  public static method Main(): int
    var n: int := 41;
    var k := new K(lambda(): int => n);
    return 0;
  end;
end;
