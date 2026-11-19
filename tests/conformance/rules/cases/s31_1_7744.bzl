//! case: s31_1_7744
//! doc: spec
//! section: 31.1
//! title: an exception from a lambda over a local is caught inside the method and the local is still readable
//! rule: a thrown exception is caught by the first arm naming its class or a base of it
//! cites: 27.5, 28.3
//! expect: run
//! stdout: 6
module Conf;
import System;
public class Program
  public static method Boom(p: int): int => throw new Exception("x");
  public static method Apply(f: () -> int): int => f();
  public static method M(p: int): int
    var q := p + 1;
    var r := 0;
    try
      r := Apply(lambda() => q + Boom(q));
    catch var e: Exception =>
      r := q;
    end try;
    return r;
  end M;
  public static method Main(): int
    var t := 0;
    for var j in 0..<3 do
      t := t + Program.M(j);
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
