//! case: s31_1_7743
//! doc: spec
//! section: 31.1
//! title: an exception from a lambda over a borrowed parameter is caught by the matching arm
//! rule: a thrown exception is caught by the first arm naming its class or a base of it
//! cites: 27.5, 28.3
//! expect: run
//! stdout: 3
module Conf;
import System;
public class Box
  public property V: int;
  public constructor(v: int) this.V := v; end constructor;
end Box;
public class Program
  public static method Boom(p: int): int => throw new Exception("x");
  public static method Apply(f: () -> int): int => f();
  public static method M(b: Box): int => Apply(lambda() => b.V + Boom(b.V));
  public static method Main(): int
    var t := 0;
    for var j in 0..<3 do
      try
        var b := new Box(j);
        t := t + Program.M(b);
      catch var e: Exception =>
        t := t + 1;
      end try;
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
