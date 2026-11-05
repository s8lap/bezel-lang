//! case: s7_5_0911
//! doc: spec
//! section: 7.5
//! title: LINQ lambdas after ?.Split(...)?. see string elements, with a nullable capture, Sum and Any
//! rule: a suffix to the right of a `?.` is typed at the value it meets on the path where it runs
//!   — the preceding segment's own declared type, never the nullable type the `?.` gives the
//!   chain's result
//! cites: 7.7
//! expect: run
//! output: 3
//!   BC
//!   5
//!   yes
//! issue: 8166
module Conf;

public class Program
  public static method Main()
    var s: string? := "a,bc,dc";
    var t: string? := "x";
    var r := s?.Split(",")?.Select(lambda(x) => t?.Length ?? 0)?.ToArray();
    Console.WriteLine(r!.Count);
    var r3 := s?.Split(",")?.Select(lambda(x) => x.ToUpper())?.ToArray();
    Console.WriteLine(r3![1]);
    var r4 := s?.Split(",")?.Select(lambda(x) => x.Length)?.Sum();
    Console.WriteLine(r4 ?? -1);
    var r5 := s?.Split(",")?.Any(lambda(x) => x.Length > 1);
    if r5 ?? false then
      Console.WriteLine("yes");
    end if;
  end Main;
end Program;
