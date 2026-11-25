//! case: l4_19_9304
//! doc: stdlib
//! section: 4.19
//! title: FrozenArray Length is an int, so it is not implicitly a string
//! rule: public readonly property Length: int;
//! cites: 4.19, 5.1
//! expect: reject
//! diagnostic: convert
//! notes: Harvested from the #9185 review (r1). `var x: string := f.Length` initializes a string from an int,
//!   which no implicit conversion (spec 5.1) allows. Deletion test / accepting twin: l4_19_9302 binds
//!   `f.Length` to an int.
module Conf;
public class Program
  public static method Main(): int
    using var s := new Array<int>();
    s.Add(7);
    using var f := FrozenArray<int>.From(s);
    var x: string := f.Length;
    return 0;
  end Main;
end Program;
