//! case: s31_1_7745
//! doc: spec
//! section: 31.1
//! title: an exception raised through a recursive chain of calls is caught by the first matching arm
//! rule: a thrown exception is caught by the first arm naming its class or a base of it
//! cites: 27.5, 28.3
//! expect: run
//! stdout: 3
module Conf;
import System;
public class Program
  public static method Boom(p: int): int => throw new Exception("x");
  public static method D(f: () -> int, n: int): int => (if n = 0 then f() else D(f, n - 1) + 1);
  public static method M(p: int): int => D(lambda() => p + Boom(p), 5);
  public static method Main(): int
    var t := 0;
    for var j in 0..<3 do
      try
        t := t + Program.M(j);
      catch e: Exception =>
        t := t + 1;
      end try;
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
