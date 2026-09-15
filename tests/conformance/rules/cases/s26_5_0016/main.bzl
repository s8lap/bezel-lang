//! case: s26_5_0016
//! doc: spec
//! section: 26.5
//! title: narrowing the import to one candidate module resolves the ambiguity
//! rule: The remedies for extension ambiguity are the mechanisms that exist:
//!   remove or narrow a conflicting import. Importing only `ExtA` leaves one
//!   candidate, so the call resolves.
//! cites: 26.5
//! expect: run
//! stdout: from-a
module App;
import Shapes;
import ExtA;

public class Program
  public static method Main()
    using var s := new Shape();
    Console.WriteLine(s.Describe());
  end Main;
end Program;
