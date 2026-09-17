//! case: l4_10_0004
//! doc: stdlib
//! section: 4.10
//! title: OwningSet<T>.Pop finds the equal element and transfers ownership out
//! rule: Pop(query: T): owned T; // finds the equal element in the set,
//!   removes it, and returns ownership. The query and the stored element
//!   are equal by the set's Equals rule but may be distinct instances.
//! cites: 4.10, 28.11
//! expect: run
//! stdout: still alive
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
    pool.Add(new Widget(1));
    using var query := new Widget(1);
    using var found := pool.Pop(query);
    Console.WriteLine("still alive");
  end Main;
end Program;
