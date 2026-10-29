//! case: s22_5_0035
//! doc: spec
//! section: 22.5
//! title: An exception from a reordered named constructor argument propagates; no object is built
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as
//!   written, for positional and named arguments alike; naming an argument changes which parameter it
//!   binds to, never when it runs. An argument expression
//!   that throws ends the call before it begins: no object is constructed and the exception is the call's
//!   outcome, reordered or not, generic enclosing method or not.
//! cites: 22.5, 31
//! expect: run
//! output: caught=400 ob2=4:9 gen=2
module Conf;

import System.Serialization;
import System.Threading;

public class Program
  public static method Num(i: int): string
    return $"{i}";
  end Num;

  public static method Boom(): int
    throw new InvalidOperationException("boom");
  end Boom;

  public static method BoomO(): JsonOptions
    throw new InvalidOperationException("boom");
  end BoomO;

  public static method Make<T>(n: T): int
    using var s := new Semaphore(maxCount := 6, initialCount := 3);
    using var t1 := s.Acquire();
    return 1;
  end Make;

  public static method Main()
    var caught := 0;
    for var i in 0..<100 do
      try
        using var d := new JsonDeserializer(input := Num(i), options := BoomO());
        Console.WriteLine("unreachable");
      catch InvalidOperationException =>
        caught := caught + 1;
      end try;
      try
        using var d2 := new JsonDeserializer(Num(i), BoomO());
        Console.WriteLine("unreachable");
      catch InvalidOperationException =>
        caught := caught + 1;
      end try;
      try
        using var s := new Semaphore(maxCount := Boom(), initialCount := 1);
        Console.WriteLine("unreachable");
      catch InvalidOperationException =>
        caught := caught + 1;
      end try;
      try
        using var ob := new OwningBuffer<int>(init := lambda(k) => k, length := Boom());
        Console.WriteLine("unreachable");
      catch InvalidOperationException =>
        caught := caught + 1;
      end try;
    end;
    using var inner := new OwningBuffer<int>(init := lambda(k) => 7 + k, length := 2);
    using var outer := new Semaphore(maxCount := 9, initialCount := inner[0] - 6);
    using var tk := outer.Acquire();
    using var ob2 := new OwningBuffer<int>(init := lambda(k) => k * 3, length := new OwningBuffer<int>(init := lambda(q) => 4, length := 1)[0]);
    Console.WriteLine($"caught={caught} ob2={ob2.Length}:{ob2[3]} gen={Make<int>(1) + Make<string>("x")}");
  end Main;
end Program;
