//! case: s17_6_9602
//! doc: spec
//! section: 17.6
//! title: decrementing FrozenArray's read-only Length is rejected
//! rule: Read-only locations cannot be incremented.
//! cites: 17.6, 23.1
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
    f.Length--;
  end Main;
end Program;
