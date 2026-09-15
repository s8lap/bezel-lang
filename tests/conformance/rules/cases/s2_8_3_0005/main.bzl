//! case: s2_8_3_0005
//! doc: spec
//! section: 2.8.3
//! title: an is type test naming a transitive type requires its own import
//! rule: Naming the type in source — including a pattern/`is` type test —
//!   requires importing its declaring module.
//! cites: 2.8.3, C.13
//! expect: reject
//! diagnostic: Money
module App;
import Lib1;

public class Program
  public static method Main()
    using var pay := Payroll.Net(1, 1);
    if pay is Money then
      Console.WriteLine("yes");
    end if;
  end Main;
end Program;
