//! case: s15_4_0003
//! doc: spec
//! section: 15.4
//! title: a sequence literal targeting Array<T> stores elements in textual order
//! rule: For `Array<T>`, elements are stored in textual order; index 0 is the
//!   first literal element (§15.4).
//! cites: 15.4
//! expect: run
//! stdout: 30
module Conf;

public class Program
  public static method Main()
    using var a: Array<int> := [10, 20, 30];
    Console.WriteLine(a[2]);
  end Main;
end Program;
