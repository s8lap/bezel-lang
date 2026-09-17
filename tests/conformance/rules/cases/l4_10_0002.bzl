//! case: l4_10_0002
//! doc: stdlib
//! section: 4.10
//! title: OwningSet<T>.Add silently disposes the item on a duplicate
//! rule: When the caller passes owned item and the set already contains
//!   an equal element, Add returns () and disposes the passed-in item —
//!   the caller relinquished ownership at the call site.
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
    pool.Add(new Widget(1));
    pool.Add(new Widget(1));
    Console.WriteLine("after second add");
  end Main;
end Program;
