//! case: l1_0008
//! doc: stdlib
//! section: 1
//! title: First() on an empty collection passes an IFailure arm
//! rule: A class whose condition can come from the program itself, or from data
//!   whose supplier the library cannot see, is not marked, and makes no
//!   statement: text that does not parse and a document that does not
//!   deserialize may be a user's typing or the program's own constant
//!   (`FormatException`, `SerializationException`); an argument, the state of
//!   an object the program holds and the program's arithmetic are the program's
//!   (`ArgumentException`, `InvalidOperationException`, `KeyNotFoundException`,
//!   `OverflowException`).
//! cites: 1, 5
//! expect: run
//! stdout: empty collection passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<int>();
    try
      try
        _ := xs.First();
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      InvalidOperationException => Console.WriteLine("empty collection passed")
    end;
  end Main;
end Program;
