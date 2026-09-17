//! case: s2_8_3_0002
//! doc: spec
//! section: 2.8.3
//! title: importing a transitive type's own declaring module admits naming it
//! rule: Naming a transitively-reachable type in source requires importing
//!   its declaring module. Once `Base` is imported, `Money` may be named in
//!   a type ascription exactly as any other visible type.
//! cites: 2.8.3, C.13
//! expect: run
//! stdout: 200
module App;
import Lib1;
import Base;

public class Program
  public static method Main()
    using var m: Money := Payroll.Net(2, 100);
    Console.WriteLine(m.Cents);
  end Main;
end Program;
