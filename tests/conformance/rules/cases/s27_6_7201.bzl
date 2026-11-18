//! case: s27_6_7201
//! doc: spec
//! section: 27.6
//! title: an extension Object reference over a capture-bearing function
//!   value is rejected at the formation
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
  public static method Main()
    using var payload := new Res();
    var f: () -> int := lambda() => payload.Use();
    var g: () -> int := f.Hold;
    Console.WriteLine(g());
  end Main;
end Program;
