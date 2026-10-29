//! case: s28_3_0291
//! doc: spec
//! section: 28.3
//! title: an alias of an owned Set of strings reads the owner's elements
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 1005
module Conf;
class Program
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      var s: Set<string> := ["a","b"];
      var t := s;
      var n: Set<string>? := s;
      sum := sum + t.Count + (n?.Count ?? 0);
      if t.Contains("a") then sum := sum + 1; end;
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
