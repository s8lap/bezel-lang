//! case: s29_1_0006
//! doc: spec
//! section: 29.1
//! title: IEnumerator.Current is non-destructive across repeated reads
//! rule: `IEnumerator<T>.Current` is non-destructive: it returns a borrow of
//!   the most-recently-yielded value, which the enumerator continues to own.
//!   Multiple reads between `MoveNext` calls produce additional borrows of the
//!   same value.
//! cites: 29.1, 23.5
//! expect: run
//! output: a-a
//!   disposed
module Conf;

public class Item
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Item;

public class OneEnumerator: IEnumerator<Item>
  private owned _item: Item;
  private _done: bool := false;

  public constructor()
    _item := new Item("a");
  end constructor;

  public method MoveNext(): bool
    if _done then
      return false;
    end;
    _done := true;
    return true;
  end MoveNext;

  public property Current: Item => _item;
end OneEnumerator;

public class Program
  public static method Main()
    using var e := new OneEnumerator();
    _ := e.MoveNext();
    Console.WriteLine($"{e.Current.Name}-{e.Current.Name}");
  end Main;
end Program;
