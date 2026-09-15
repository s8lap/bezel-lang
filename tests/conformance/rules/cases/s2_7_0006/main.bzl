//! case: s2_7_0006
//! doc: spec
//! section: 2.7
//! title: a type named A shadows an imported module-name prefix A for chain
//!   purposes
//! rule: Consequences worth stating — a type named `A` in scope **shadows**
//!   a module-name prefix `A.…` for chain purposes (the compiler warns when
//!   both readings exist). `Data` the locally-declared type wins over
//!   `Data` the imported module's prefix.
//! cites: 2.7
//! expect: warn
//! diagnostic: Data
module Conf;
import Data;

public class Data
  public static method Foo(): string => "from-type";
end Data;

public class Program
  public static method Main()
    Console.WriteLine(Data.Foo());
  end Main;
end Program;
