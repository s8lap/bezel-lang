//! case: l4_19_9305
//! doc: stdlib
//! section: 4.19
//! title: a lambda declared to return string cannot return FrozenArray Length
//! rule: public readonly property Length: int;
//! cites: 4.19, 27.3, 5.1
//! expect: reject
//! diagnostic: convert
//! notes: Harvested from the #9185 review (r4). `lambda(): string => f.Length` declares a string result; the
//!   body is an int and has no implicit conversion to string. The accepting shape is l4_19_9301's
//!   `lambda() => f.Length`.
module Conf;
public class Program
  public static method Main(): int
    using var s := new Array<int>();
    s.Add(7);
    using var f := FrozenArray<int>.From(s);
    var fn := lambda(): string => f.Length;
    return 0;
  end Main;
end Program;
