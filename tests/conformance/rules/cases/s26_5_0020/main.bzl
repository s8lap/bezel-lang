//! case: s26_5_0020
//! doc: spec
//! section: 26.5
//! title: the current module's own extension conflicts with an imported one
//! rule: The compiler searches the current module and all imported modules
//!   for extension blocks. `App`'s own block on `Shape` and the imported
//!   `ExtShape` block are equally specific (identical substituted heads), so
//!   the call is ambiguous (rule 4) exactly as if both candidates were
//!   imported.
//! cites: 26.5
//! expect: reject
//! diagnostic: Describe
module App;
import Shapes;
import ExtShape;

extension Shape
  public method Describe(): string => "local";
end;

public class Program
  public static method Main()
    using var s := new Shape();
    Console.WriteLine(s.Describe());
  end Main;
end Program;
