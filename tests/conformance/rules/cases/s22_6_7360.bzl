//! case: s22_6_7360
//! doc: spec
//! section: 22.6
//! title: a lambda whose body converts to both `() -> long` and `() ->
//!   double` constructor slots is ambiguous
//! rule: two candidates that both require an implicit conversion at the same
//!   position are **equally ranked** there
//! cites: 5.1, 27.3
//! expect: reject
//! diagnostic: ambiguous
//! notes: Pins the #6642 fix: the body `41 + 1` is an int, which converts
//!   implicitly to long and to double (5.1), so each candidate converts at
//!   the lambda's position and the two are equally ranked. Before the fix the
//!   first declared constructor was chosen silently.
module Conf;

public class K
  public constructor(f: () -> long) Console.WriteLine("A:" + f().ToString()); end;
  public constructor(f: () -> double) Console.WriteLine("B"); end;
end;
public static class Program
  public static method Main(): int
    var k := new K(lambda() => 41 + 1);
    return 0;
  end;
end;
