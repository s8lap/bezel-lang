//! case: s27_6_7202
//! doc: spec
//! section: 27.6
//! title: an extension Object reference over a capture-bearing function
//!   value is rejected as an argument
//! rule: every receiver-side rule of the call the formation resolves as is
//!   read at the formation
//! cites: 27.6, 28.3, 26.5
//! expect: reject
//! diagnostic: receiver
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
  public static method Take(h: () -> int): int => h();

  public static method Main()
    using var payload := new Res();
    var f: () -> int := lambda() => payload.Use();
    Console.WriteLine(Program.Take(f.Hold));
  end Main;
end Program;
