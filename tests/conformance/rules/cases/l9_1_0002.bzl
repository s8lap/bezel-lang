//! case: l9_1_0002
//! doc: stdlib
//! section: 9.1
//! title: int.TryParse returns false and assigns default(int) on failure
//! rule: TryParse(s: string, out result: int): bool is Try shape (c) —
//!   false on failure, with the out slot assigned default(T) (here 0).
//! cites: 9.1, 1
//! expect: run
//! output: False
//!   0
module Conf;

public class Program
  public static method Main()
    var ok := int.TryParse("nope", out var n);
    Console.WriteLine(ok);
    Console.WriteLine(n);
  end Main;
end Program;
