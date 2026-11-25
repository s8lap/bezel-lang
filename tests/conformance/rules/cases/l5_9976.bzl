//! case: l5_9976
//! doc: stdlib
//! section: 5
//! title: bare instance method names in a chained Where and Select, run through an interface receiver
//! rule: is **retained by the returned query value until enumeration**, not just
//!   for the duration of the call
//! cites: 5, 27.6, 2.7
//! expect: run
//! issue: 9292
//! stdout: 1000
module Conf;

public interface IRunner
  method Run(): int;
end IRunner;

public class Runner: IRunner
  public method Keep(x: int): bool => x > 1;
  public method Conv(x: int): int => x * 2;

  public method Run(): int
    using var xs: Array<int> := [1, 2, 3];
    var t := 0;
    for var y in xs.Where(Keep).Select(Conv) do
      t := t + y;
    end;
    return t;
  end Run;
end Runner;

public class Program
  public static method Main()
    var total := 0;
    for var j in 0 ..< 100 do
      using var r: IRunner := new Runner();
      total := total + r.Run();
    end;
    Console.WriteLine(total);
  end Main;
end Program;
