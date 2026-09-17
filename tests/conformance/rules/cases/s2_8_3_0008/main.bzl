//! case: s2_8_3_0008
//! doc: spec
//! section: 2.8.3
//! title: the dependency closure reaches an arbitrarily deep transitive chain
//! rule: A build must present the artifacts of the root compilation's direct
//!   imports and, transitively, of every module those artifacts' interfaces
//!   reference. `App` imports only `Outer`; `Money` is two hops further away
//!   (`Outer` -> `Mid` -> `Base`), and using it needs no import at any level.
//! cites: 2.8.3, C.13
//! expect: run
//! stdout: 77
module App;
import Outer;

public class Program
  public static method Main()
    using var m := Facade.Build(77);
    Console.WriteLine(m.Cents);
  end Main;
end Program;
