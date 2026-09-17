//! case: s33_1_0001
//! doc: spec
//! section: 33.1
//! title: a qualifying Main in a dependency artifact is inert
//! rule: A public static method Main declared on a class in the root
//!   compilation is the entry point. A qualifying signature in a
//!   dependency artifact is inert — never selected, never a conflict, an
//!   ordinary method. `Dep` even declares two qualifying overloads, which
//!   would be a §33.5 ambiguity error in a root compilation, yet it
//!   compiles as a dependency and never perturbs `App`'s build.
//! cites: 33.1, 2.8.4, 33.5
//! notes: dep-main
//! expect: run
//! stdout: root
module App;
import Dep;

public class Program
  public static method Main()
    Console.WriteLine("root");
  end Main;
end Program;
