//! case: l4_15_0005
//! doc: stdlib
//! section: 4.15
//! title: ArraySpan<T>.Fill assigns the value to every slot, for class and value T alike
//! rule: Unlike Span<T> (whose mutation API splits by T's category),
//!   ArraySpan<T> provides a single uniform mutation surface for all T --
//!   admissible because Array<T> slots do not own their contents.
//! cites: 4.15
//! expect: run
//! stdout: 9 9 9
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    var s := xs.AsSpan();
    s.Fill(9);
    Console.WriteLine($"{xs[0]} {xs[1]} {xs[2]}");
  end Main;
end Program;
