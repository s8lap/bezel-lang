//! case: l9_6_0024
//! doc: stdlib
//! section: 9.6
//! title: string.IndexOf with a startIndex equal to the receiver's Length answers -1
//! rule: public pure method IndexOf(value: char, startIndex: int := 0): int;
//!   code-point index of the first occurrence at or after startIndex, or -1;
//!   0 <= startIndex <= Length (IndexOutOfRangeException otherwise) — a
//!   startIndex equal to the receiver's Length is inside the bound, and the
//!   occurrence lies before it, so the answer is -1.
//! cites: 9.6
//! expect: run
//! stdout: -1
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("abcdef".IndexOf('c', 6));
  end Main;
end Program;
