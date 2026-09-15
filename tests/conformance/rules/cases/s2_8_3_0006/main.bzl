//! case: s2_8_3_0006
//! doc: spec
//! section: 2.8.3
//! title: an explicit generic type argument naming a transitive type requires its own import
//! rule: Naming the type in source — including an explicit (source-spelled)
//!   generic type argument — requires importing its declaring module.
//! cites: 2.8.3, C.13
//! expect: reject
//! diagnostic: Money
module App;
import Lib1;

public class Program
  public static method Main()
    using var box := new OwningArray<Money>();
    Console.WriteLine(box.Count);
  end Main;
end Program;
