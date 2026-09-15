//! case: l2_13_0002
//! doc: stdlib
//! section: 2.13
//! title: Type.FullName renders the module-qualified path
//! rule: FullName renders every named component qualified by its declaring
//!   module path — for a top-level type, the module then the identifier
//!   ("BankingApp.BankAccount").
//! cites: 2.13
//! expect: run
//! stdout: Conf.Widget
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    Console.WriteLine(typeof(Widget).FullName);
  end Main;
end Program;
