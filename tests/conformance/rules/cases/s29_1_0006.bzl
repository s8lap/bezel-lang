//! case: s29_1_0006
//! doc: spec
//! section: 29.1
//! title: IEnumerator.Current is non-destructive across repeated reads
//! rule: `IEnumerator<T>.Current` is non-destructive: it returns a borrow of
//!   the most-recently-yielded value ... Multiple reads between `MoveNext`
//!   calls produce additional borrows of the same value.
//! cites: 29.1
//! expect: run
//! stdout: 0-0
module Conf;

public class TrioEnumerator: IEnumerator<int>
  private _i: int := -1;

  public constructor()
  end constructor;

  public method MoveNext(): bool
    _i := _i + 1;
    return _i < 3;
  end MoveNext;

  public property Current: int => _i;
end TrioEnumerator;

public class Trio: IEnumerable<int>
  public constructor()
  end constructor;

  public method GetEnumerator(): owned IEnumerator<int>
    return new TrioEnumerator();
  end GetEnumerator;
end Trio;

public class Program
  public static method Main()
    var source := new Trio();
    using var e := source.GetEnumerator();
    _ := e.MoveNext();
    Console.WriteLine($"{e.Current}-{e.Current}");
  end Main;
end Program;
