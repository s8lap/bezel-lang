//! case: s23_1_9601
//! doc: spec
//! section: 23.1
//! title: assigning FrozenArray's read-only Length is rejected
//! rule: A `readonly` auto-property may be assigned only in a field initializer or
//!   in a constructor
//! cites: 23.1, 17.6
//! expect: reject
//! diagnostic: read-only
//! issue: 9214
module Conf;

public class Program
  public static method Main()
    var s := new Array<int>();
    s.Add(7);
    s.Add(2);
    using var f := FrozenArray<int>.From(s);
    f.Length := 3;
  end Main;
end Program;
