//! case: s2_8_2_7372
//! doc: spec
//! section: 2.8.2
//! title: an imported generic bound through an IList formal fed an Array<int>
//!   runs as in-module
//! rule: An external instantiation behaves identically to a same-module
//!   instantiation of the same generic at the same arguments
//! cites: 14.5, 14.1
//! expect: run
//! stdout: 7
//! notes: Pins the #6828 fix's admitting side (a base-green control, so it
//!   lives in the corpus and not in gtest): the imported generic `ShowL`
//!   binds `T` at `int` (or at a shape built from `int`), which is exactly
//!   the binding the defining module's template was compiled against, so the
//!   call runs and prints the value. Array<int> implements IList<int>.
module Conf;
import Lib;
public class Program
  public static method Main(): int
    var xs := new Array<int>();
    xs.Add(7);
    Console.WriteLine(Lib.G.ShowL(xs));
    return 0;
  end Main;
end Program;
