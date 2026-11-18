//! case: s2_8_2_7375
//! doc: spec
//! section: 2.8.2
//! title: an imported generic bound through two function-typed formals at
//!   `int` runs as in-module
//! rule: An external instantiation behaves identically to a same-module
//!   instantiation of the same generic at the same arguments
//! cites: 14.5, 14.1
//! expect: run
//! stdout: 12
//! notes: Pins the #6828 fix's admitting side (a base-green control, so it
//!   lives in the corpus and not in gtest): the imported generic `ShowFS`
//!   binds `T` at `int` (or at a shape built from `int`), which is exactly
//!   the binding the defining module's template was compiled against, so the
//!   call runs and prints the value. Both formals bind the same `T := int`;
//!   the result concatenates 1 and 2.
module Conf;
import Lib;
public class Program
  public static method Main(): int
    Console.WriteLine(Lib.G.ShowFS(lambda() => 1, lambda() => 2));
    return 0;
  end Main;
end Program;
