//! case: s28_3_11_4702
//! doc: spec
//! section: 28.3.11
//! title: an async method returning a lazy query over its parameter is admitted and enumerated by the caller
//! rule: provided every borrow the returned frame carries is sourced from the method's own parameters or
//!   receiver
//! cites: 28.3, 30.1, 5
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static async method Evens(xs: Array<int>): Task<IEnumerable<int>>
    return xs.Where(lambda(x) => x > 1);
  end Evens;

  public static async method Main(): Task
    var shared: Array<int> := [1, 2, 3];
    Console.WriteLine((await Program.Evens(shared)).Sum());
  end Main;
end Program;
