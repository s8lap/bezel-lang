//! case: s15_1_1109
//! doc: spec
//! section: 15.1
//! title: the *OrNull tier over a record that merely contains an inline array is admitted
//! rule: the `*OrNull` tier (`FirstOrNull`) ... is a conditionally-admitted member where that
//!   type argument is an inline array
//! cites: 14.2
//! expect: run
//! output: 2
//!   3
//!   1
//! issue: 5911
module Conf;

public record R(arr: [3]int);

public class Program
  public static method Main()
    var a: [3]int := [1, 2, 3];
    using var xs := new Array<R>();
    xs.Add(new R(a));
    Console.WriteLine(xs.FirstOrNull()!.arr[1]);
    Console.WriteLine(xs.LastOrNull()!.arr[2]);
    Console.WriteLine(xs.Select(lambda(r: R) => r.arr[0]).FirstOrNull()!);
  end Main;
end Program;
