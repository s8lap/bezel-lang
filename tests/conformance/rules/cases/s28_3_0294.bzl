//! case: s28_3_0294
//! doc: spec
//! section: 28.3
//! title: an alias of an owned Array of borrowed class elements reads the elements
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 1005
module Conf;
class Item public _v: int := 1; end Item;
class Program
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      using var it := new Item();
      var a: Array<Item> := [it, it];
      var b := a;
      var n: Array<Item>? := a;
      sum := sum + b.Count + (n?.Count ?? 0) + b[0]._v;
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
