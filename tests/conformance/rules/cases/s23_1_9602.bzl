//! case: s23_1_9602
//! doc: spec
//! section: 23.1
//! title: assigning Length of a FrozenArray read through a property is rejected
//! rule: A `readonly` auto-property may be assigned only in a field initializer or
//!   in a constructor
//! cites: 23.1, 17.6
//! expect: reject
//! diagnostic: read-only
//! issue: 9214
module Conf;

public class Holder
  private _f: FrozenArray<int>;

  public constructor(f: FrozenArray<int>)
    this._f := f;
  end constructor;

  public property Items: FrozenArray<int> => this._f;
end Holder;

public class Program
  public static method Main()
    var s := new Array<int>();
    s.Add(7);
    using var f := FrozenArray<int>.From(s);
    var h := new Holder(f);
    h.Items.Length := 3;
  end Main;
end Program;
