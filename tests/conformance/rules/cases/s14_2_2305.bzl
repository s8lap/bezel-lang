//! case: s14_2_2305
//! doc: spec
//! section: 14.2
//! title: Math.Max over a user record that declares no IComparable is
//!   refused
//! rule: Non-nullable types conforming to `IFoo`: classes implementing it,
//!   records and enums implementing it, interfaces extending it
//! cites: 2.2
//! expect: reject
//! diagnostic: constraint
//! notes: Green on base and tip (control: the ordinary constraint check
//!   already rejected user records).
module Conf;

public record Pt(X: int);

public class Program
  public static method Main()
    var a := new Pt(1);
    var m := Math.Max(a, a);
  end Main;
end Program;
