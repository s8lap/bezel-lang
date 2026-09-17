//! case: s2_8_3_0004
//! doc: spec
//! section: 2.8.3
//! title: a new expression naming a transitive type requires its own import
//! rule: Naming the type in source — including in a `new` expression —
//!   requires importing its declaring module, even though `Money` is
//!   otherwise reachable through `Lib1`'s surface without one.
//! cites: 2.8.3, C.13
//! expect: reject
//! diagnostic: Money
module App;
import Lib1;

public class Program
  public static method Main()
    using var m := new Money(500);
    Console.WriteLine(m.Cents);
  end Main;
end Program;
