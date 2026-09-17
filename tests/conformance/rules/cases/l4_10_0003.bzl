//! case: l4_10_0003
//! doc: stdlib
//! section: 4.10
//! title: OwningSet<T>.TryAdd disposes the passed item on a duplicate
//! rule: method TryAdd(owned item: T): bool; // returns whether added
//!   (false if duplicate; passed item is disposed).
//! cites: 4.10, 28.11
//! expect: run
//! stdout: widget disposed
module Conf;

public class Widget
  private _key: int;

  public constructor(key: int)
    _key := key;
  end constructor;

  public override method Equals(other: Object?): bool
    var w := other as Widget;
    if w = null then
      return false;
    end;
    return w._key = _key;
  end Equals;

  public override method GetHashCode(): int => _key;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningSet<Widget>();
    _ := pool.TryAdd(new Widget(1));
    var ok := pool.TryAdd(new Widget(1));
    Console.WriteLine(ok);
  end Main;
end Program;
