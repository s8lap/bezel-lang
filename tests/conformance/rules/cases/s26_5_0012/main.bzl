//! case: s26_5_0012
//! doc: spec
//! section: 26.5
//! title: importing an extension's own module makes it reachable
//! rule: Extension members are candidates only from the current module and
//!   imported modules. Once `App` also imports `Lib`, `Doubled` is a
//!   candidate for the `Widget` receiver.
//! cites: 26.5, 2.8.3
//! expect: run
//! stdout: 12
module App;
import Factory;
import Lib;

public class Program
  public static method Main()
    using var w := WidgetFactory.Make(6);
    Console.WriteLine(w.Doubled());
  end Main;
end Program;
