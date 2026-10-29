//! case: s28_3_0293
//! doc: spec
//! section: 28.3
//! title: an alias of an owned Array of strings reads the owner's elements
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 1407
module Conf;
class Program
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      var a: Array<string> := ["x","yy"];
      var b := a;
      var n: Array<string>? := a;
      sum := sum + b[1].Length + (n?.Count ?? 0);
      for var s in b do sum := sum + s.Length; end;
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
