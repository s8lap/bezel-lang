//! case: l3_1_0053
//! doc: stdlib
//! section: 3.1
//! title: an Object-typed argument to a StringBuilder member's string parameter is rejected
//! rule: stdlib §3.1 declares `Replace(oldValue: string, newValue: string): StringBuilder`.
//!   §5.1's reference conversions run from a type to a WIDER one - class to base
//!   class, transitively up to `Object`, and any type to `Object` - never from
//!   `Object` to a narrower one, so an `Object` reference converts to no `string`.
//!   No candidate of §22.6 is applicable and the call is a compile error.
//! cites: 3.1, 5.1, 22.6
//! expect: reject
//! diagnostic: not implicitly convertible
//! issue: 7207
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("hello");
    var o: Object := "l";
    _ := sb.Replace(o, "L");
  end Main;
end Program;
