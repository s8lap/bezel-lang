//! case: s27_7_1_0005
//! doc: spec
//! section: 27.7.1
//! title: a Set of Object holds two evaluations of one static method reference as one element
//! rule: A function value's `GetHashCode` **agrees with this equality** ...
//!   The hash ... holds at `Object`, where a capture-free function value is
//!   admitted ... and is an ordinary key: a `Set<Object>` holds two
//!   evaluations of one static method's reference as one element
//! cites: 27.7.1, 8.3, 3.6
//! expect: run
//! output: 1
//!   2
//! issue: 5905
module Conf;

public class Demo
  public static method Twice(x: int): int => x * 2;

  public static method Main()
    var f: (int) -> int := Demo.Twice;
    var g: (int) -> int := Demo.Twice;
    var refs := new Set<Object>();
    refs.Add(f);
    refs.Add(g);
    Console.WriteLine(refs.Count);
    var l1: (int) -> int := lambda(x) => x * 2;
    var l2: (int) -> int := lambda(x) => x * 2;
    var lams := new Set<Object>();
    lams.Add(l1);
    lams.Add(l2);
    Console.WriteLine(lams.Count);
  end Main;
end Demo;
