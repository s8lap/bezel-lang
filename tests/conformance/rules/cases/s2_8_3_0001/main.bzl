//! case: s2_8_3_0001
//! doc: spec
//! section: 2.8.3
//! title: a type ascription naming a transitive type requires its own import
//! rule: Naming the type in source — in a type ascription, parameter or
//!   return type, `new` expression, cast, `is`/pattern type test, `catch`
//!   clause, explicit generic type argument, or extension declaration —
//!   requires importing its declaring module, even though `Money` reaches
//!   `App`'s compilation only transitively through `Lib1`.
//! cites: 2.8.3, C.13
//! expect: reject
//! diagnostic: Money
module App;
import Lib1;

public class Program
  public static method Main()
    using var m: Money := Payroll.Net(1, 1);
    Console.WriteLine(m.Cents);
  end Main;
end Program;
