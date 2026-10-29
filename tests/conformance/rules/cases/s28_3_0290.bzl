//! case: s28_3_0290
//! doc: spec
//! section: 28.3
//! title: an alias of an owned Dictionary iterates and indexes the owner's entries
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 1206
module Conf;
class Program
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      var d: Dictionary<string,int> := ["a":1,"b":2];
      var e := d;
      var n: Dictionary<string,int>? := d;
      for var kv in e do sum := sum + kv.Value; end;
      sum := sum + (n?.Count ?? 0);
      sum := sum + e["a"];
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
