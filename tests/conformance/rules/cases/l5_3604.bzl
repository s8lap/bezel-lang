//! case: l5_3604
//! doc: stdlib
//! section: 5
//! title: SingleOrNull(pred) with exactly one match returns it
//! rule: `Single(pred)` the sole matching element … `SingleOrNull(pred)` return `null` when none matches
//!   (`SingleOrNull(pred)` still raises when more than one matches)
//! cites: 5
//! expect: run
//! stdout: 20
//! issue: 9287
module Conf;

public class Program
  public static method Main()
    using var a: Array<int> := [10, 20, 30];
    Console.WriteLine(a.SingleOrNull(lambda(x: int) => x = 20)!);
  end Main;
end Program;
