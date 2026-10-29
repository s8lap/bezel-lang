//! case: s28_3_0295
//! doc: spec
//! section: 28.3
//! title: aliases of an OwningArray run each element's destructor exactly once, at the owner's disposal
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 400 200
module Conf;
class Item
  public static _d: int := 0;
  destructor Item._d := Item._d + 1; end destructor;
end Item;
class Program
  public static method Main(): int
    var sum := 0;
    for var i in 0..99 do
      using var a: OwningArray<Item> := [new Item(), new Item()];
      var b := a;
      var c: OwningArray<Item>? := a;
      sum := sum + b.Count + (c?.Count ?? 0);
    end;
    Console.WriteLine($"{sum} {Item._d}");
    return 0;
  end Main;
end Program;
