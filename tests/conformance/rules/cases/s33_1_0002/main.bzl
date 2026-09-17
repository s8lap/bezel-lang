//! case: s33_1_0002
//! doc: spec
//! section: 33.1
//! title: a dependency's Main stays inert even when its class is used by the root
//! rule: Every Bezel executable has exactly one entry point: a
//!   `public static method Main` declared on a class in the root compilation.
//!   A qualifying signature in a dependency artifact is inert — never
//!   selected, never a conflict, an ordinary method — and that holds for a
//!   dependency class the root compilation actually references, not only for
//!   one it merely imports.
//! cites: 33.1, 2.8.4, 33.5
//! notes: dep-main
//! expect: run
//! stdout: worker
module App;
import Dep;

public class Program
  public static method Main()
    Console.WriteLine(Worker.Label());
  end Main;
end Program;
