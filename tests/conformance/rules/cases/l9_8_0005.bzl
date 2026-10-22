//! case: l9_8_0005
//! doc: stdlib
//! section: 9.8
//! title: Range.Contains in a lambda is correct for long, uint, ulong, descending, stepped, empty ranges
//! rule: Stdlib §9.8 (v0.340): "`Contains(v)` is `true` iff the range is nonempty under iteration and `v` lies within
//!   the endpoint extent (exclusive upper bound excluded) ... `Step` affects membership only through
//!   direction/emptiness." So `1L..10L step 3L` contains 5L (the step skips it in iteration, not in
//!   membership), `10..1 step -3` contains 8, and `5..<5` is empty and contains nothing.
//! cites: 16.3
//! expect: run
//! output: True
//!   True
//!   True
//!   False
//!   True
//!   False
//!   True
//!   True
//!   False
module Conf;

public class Program
  public static method Main()
    var a := 1L..10L step 3L;
    var f := lambda(x: long) => a.Contains(x);
    Console.WriteLine(f(4L));
    Console.WriteLine(f(5L));
    var b := 1u..4u;
    var g := lambda(x: uint) => b.Contains(x);
    Console.WriteLine(g(4u));
    Console.WriteLine(g(5u));
    var c := 1UL..4UL;
    var h := lambda(x: ulong) => c.Contains(x);
    Console.WriteLine(h(2UL));
    Console.WriteLine(h(9UL));
    var e := 10..1 step -3;
    var j := lambda(x: int) => e.Contains(x);
    Console.WriteLine(j(7));
    Console.WriteLine(j(8));
    var k := 5..<5;
    var l := lambda(x: int) => k.Contains(x);
    Console.WriteLine(l(5));
  end Main;
end Program;
