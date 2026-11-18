//! case: s27_6_7203
//! doc: spec
//! section: 27.6
//! title: a receiver-qualified extension Object reference over a
//!   capture-free function value is admitted
//! rule: every receiver-side rule of the call the formation resolves as is
//!   read at the formation
//! cites: 27.6, 28.3, 26.5
//! expect: run
//! stdout: 1
module Conf;

public class Res
  public constructor()
  end constructor;

  public method Use(): int => 3;
end Res;

public extension Object
  public method Hold(): int => 1;
end extension;

public class Program
  public static method Main()
    var f: () -> int := lambda() => 5;
    var g: () -> int := f.Hold;
    Console.WriteLine(g());
  end Main;
end Program;
