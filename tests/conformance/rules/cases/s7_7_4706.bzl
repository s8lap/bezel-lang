//! case: s7_7_4706
//! doc: spec
//! section: 7.7
//! title: ??= through a ref head, an indexer head and a static property head assigns when null
//! rule: `x ??= e` assigns `e` only when `x`'s current value is null, with the typing and ownership rules
//!   of
//! cites: 6.6, 7.7
//! expect: run
//! output: x
//!   5
//!   y
module Conf;

public class Bag
  private owned _items: Array<string?>;

  public constructor()
    this._items := new Array<string?>(2, null);
  end constructor;

  public property [i: int]: string?
    get => this._items[i];
    set => this._items[i] := value;
  end;

  public static property SP: int?;
end Bag;

public class Program
  public static method Fill(ref s: string?, v: string)
    s ??= v;
  end Fill;

  public static method Main()
    using var b := new Bag();
    b[0] ??= "x";
    Bag.SP ??= 5;
    var s: string? := null;
    Program.Fill(ref s, "y");
    Console.WriteLine(b[0] ?? "none");
    Console.WriteLine(Bag.SP ?? 0);
    Console.WriteLine(s ?? "none");
  end Main;
end Program;
