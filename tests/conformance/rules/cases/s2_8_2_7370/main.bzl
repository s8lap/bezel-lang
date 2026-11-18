//! case: s2_8_2_7370
//! doc: spec
//! section: 2.8.2
//! title: an imported generic bound through a function-typed formal at `int`
//!   runs as in-module
//! rule: An external instantiation behaves identically to a same-module
//!   instantiation of the same generic at the same arguments
//! cites: 14.5, 14.1
//! expect: run
//! stdout: 5
//! notes: Pins the #6828 fix's admitting side (a base-green control, so it
//!   lives in the corpus and not in gtest): the imported generic `ShowF`
//!   binds `T` at `int` (or at a shape built from `int`), which is exactly
//!   the binding the defining module's template was compiled against, so the
//!   call runs and prints the value. The function-typed formal binds `T :=
//!   int` component-wise (14.5 phase 1).
module Conf;
import Lib;
public class Program
  public static method Main(): int
    Console.WriteLine(Lib.G.ShowF(lambda() => 5));
    return 0;
  end Main;
end Program;
