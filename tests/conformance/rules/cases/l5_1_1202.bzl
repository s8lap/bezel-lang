//! case: l5_1_1202
//! doc: stdlib
//! section: 5.1
//! title:no-selector Min/Max on a Stack and a lazy Where over Stack and Set return the element
//! rule:`Min` and `Max` without a selector are generic over comparable types:
//! cites: 5
//! expect: run
//! output: b
//!   x
//!   5
module Conf;

public class Program
  public static method Main()
    using var s := new Stack<string>();
    s.Push("m");
    s.Push("b");
    s.Push("x");
    Console.WriteLine(s.Min());
    Console.WriteLine(s.Where(lambda(v: string) => true).Max());
    using var t := new Set<int>();
    t.Add(5);
    t.Add(2);
    t.Add(9);
    Console.WriteLine(t.Where(lambda(v: int) => v > 2).Min());
  end Main;
end Program;
