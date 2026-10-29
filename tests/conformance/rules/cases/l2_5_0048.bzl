//! case: l2_5_0048
//! doc: stdlib
//! section: 2.5
//! title: an iterator yielding DateTime constructions raises mid-sequence and the loop sees the earlier values
//! rule: ArgumentOutOfRangeException on invalid components
//! cites: 2.5, 29.1
//! expect: run
//! output: 50
//!   3050
module Conf;

public class Program
  public static method Gen(n: int): IEnumerable<DateTime>
    for var i in 0..<n do
      yield return new DateTime(2020, 1, 30 + i);
    end;
  end Gen;
  public static method Main()
    var caught := 0;
    var ok := 0;
    for var k in 0..<50 do
      try
        for var d in Program.Gen(4) do
          ok := ok + d.Day;
        end;
      catch ArgumentException => caught := caught + 1; end;
    end;
    Console.WriteLine(caught);
    Console.WriteLine(ok);
  end Main;
end Program;
