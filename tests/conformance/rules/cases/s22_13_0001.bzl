//! case: s22_13_0001
//! doc: spec
//! section: 22.13
//! title: static fields of a single type are initialized in textual declaration order
//! rule: Within a single type, static fields are initialized in textual
//!   declaration order.
//! cites: 22.13
//! expect: run
//! output: a
//!   b
module Conf;

public class Init
  private static readonly _a: int := Announce("a");
  private static readonly _b: int := Announce("b");

  private static method Announce(name: string): int
    Console.WriteLine(name);
    return 0;
  end Announce;

  public static method Touch()
    var x := _a;
  end Touch;
end Init;

public class Program
  public static method Main()
    Init.Touch();
  end Main;
end Program;
