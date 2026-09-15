//! case: l5_0007
//! doc: stdlib
//! section: 5
//! title: Zip ends when either input sequence ends
//! rule: Zip ends when either input ends.
//! cites: 5
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    using var ys: Array<int> := [10, 20];
    using var zipped := xs.Zip(ys, lambda(a: int, b: int): int => a + b).ToArray();
    Console.WriteLine(zipped.Count);
  end Main;
end Program;
