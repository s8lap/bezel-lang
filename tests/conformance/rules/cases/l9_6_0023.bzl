//! case: l9_6_0023
//! doc: stdlib
//! section: 9.6
//! title: string.IndexOf with a startIndex past the receiver's Length raises IndexOutOfRangeException
//! rule: public pure method IndexOf(value: char, startIndex: int := 0): int;
//!   code-point index of the first occurrence at or after startIndex, or -1;
//!   0 <= startIndex <= Length (IndexOutOfRangeException otherwise)
//! cites: 9.6
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := "abcdef".IndexOf('c', 7);
    catch
      var ex: IndexOutOfRangeException => Console.WriteLine("caught");
    end try;
  end Main;
end Program;
