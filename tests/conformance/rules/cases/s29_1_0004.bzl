//! case: s29_1_0004
//! doc: spec
//! section: 29.1
//! title: a plain-var binding of a manually-obtained enumerator auto-drops with the must-use warning
//! rule: though a plain `var` binding is also legal and auto-drops at scope
//!   end with the must-use warning.
//! cites: 29.1, 28.3
//! expect: warn
//! diagnostic: must use
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
    var e := source.GetEnumerator();
    _ := e.MoveNext();
  end Main;
end Program;
