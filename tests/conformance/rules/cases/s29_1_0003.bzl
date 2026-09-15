//! case: s29_1_0003
//! doc: spec
//! section: 29.1
//! title: a manually-obtained enumerator is bound with using var and discharged
//! rule: Code that calls `GetEnumerator()` manually — outside a loop form —
//!   obtains an owned enumerator subject to the standard §28.3 discharge
//!   rules; the `using var x := source.GetEnumerator();` shape is
//!   recommended (scope-bound disposal, no must-use warning).
//! cites: 29.1
//! expect: run
//! stdout: 0
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
    Console.WriteLine(e.Current);
  end Main;
end Program;
