//! case: s2_8_3_0003
//! doc: spec
//! section: 2.8.3
//! title: use without import — an inferred binding needs no import of a transitive type
//! rule: A file that imports module `A` may freely use values of any public
//!   type reachable through `A`'s public surface — receive them into
//!   `using var` bindings with inferred types, call their public members,
//!   and dispose them — without importing the type's declaring module.
//! cites: 2.8.3, C.13
//! expect: run
//! stdout: 300
module App;
import Lib1;

public class Program
  public static method Main()
    using var pay := Payroll.Net(3, 100);
    Console.WriteLine(pay.Cents);
  end Main;
end Program;
