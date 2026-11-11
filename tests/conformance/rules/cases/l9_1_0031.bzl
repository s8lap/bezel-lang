//! case: l9_1_0031
//! doc: stdlib
//! section: 9.1
//! title: an arithmetic overflow passes an IFailure arm
//! rule: A class whose condition can come from the program itself, or from data
//!   whose supplier the library cannot see, is not marked, and makes no
//!   statement: text that does not parse and a document that does not
//!   deserialize may be a user's typing or the program's own constant
//!   (`FormatException`, `SerializationException`); an argument, the state of
//!   an object the program holds and the program's arithmetic are the program's
//!   (`ArgumentException`, `InvalidOperationException`, `KeyNotFoundException`,
//!   `OverflowException`).
//! cites: 9.1, 1
//! expect: run
//! stdout: overflow passed
//! issue: 8414
module Conf;

public class Program
  public static method Add(a: int, b: int): int => a + b;

  public static method Main()
    try
      try
        Console.WriteLine(Program.Add(int.MaxValue, 1));
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      OverflowException => Console.WriteLine("overflow passed")
    end;
  end Main;
end Program;
