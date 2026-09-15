//! case: s2_8_3_0007
//! doc: spec
//! section: 2.8.3
//! title: the same type reached through two import paths is one type
//! rule: A type is identified program-wide by its declaring module name,
//!   qualified type name, and generic arity. `App` imports `Lib1` and `Lib2`,
//!   both of which import `Base`, and `App` imports neither `Base` nor
//!   presents it twice by hand — `Money` arrives via both `Lib1`'s and
//!   `Lib2`'s interfaces and is one type: a value `Payroll.Net` produces
//!   flows into `Invoices.Total`'s parameter with no conversion.
//! cites: 2.8.3, C.13
//! expect: run
//! stdout: 1000
module App;
import Lib1;
import Lib2;

public class Program
  public static method Main()
    using var pay := Payroll.Net(40, 25);
    Console.WriteLine(Invoices.Total(pay));
  end Main;
end Program;
