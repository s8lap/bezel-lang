//! case: s26_5_0010
//! doc: spec
//! section: 26.5
//! title: an extension block declared in an imported module reaches the receiver
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules for
//!   extension blocks on the receiver's type. `Doubled` is declared in `Lib`,
//!   not in `App`, and is reached because `App` imports `Lib`.
//! cites: 26.5
//! expect: run
//! stdout: 12
module App;
import Lib;

public class Program
  public static method Main()
    using var w := new Widget(6);
    Console.WriteLine(w.Doubled());
  end Main;
end Program;
