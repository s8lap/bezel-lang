//! case: l1_0007
//! doc: stdlib
//! section: 1
//! title: a missing key at the indexer passes an IFailure arm
//! rule: A class whose condition can come from the program itself, or from data
//!   whose supplier the library cannot see, is not marked, and makes no
//!   statement: text that does not parse and a document that does not
//!   deserialize may be a user's typing or the program's own constant
//!   (`FormatException`, `SerializationException`); an argument, the state of
//!   an object the program holds and the program's arithmetic are the program's
//!   (`ArgumentException`, `InvalidOperationException`, `KeyNotFoundException`,
//!   `OverflowException`).
//! cites: 1, 4.8
//! expect: run
//! stdout: missing key passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    try
      try
        _ := d["missing"];
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      KeyNotFoundException => Console.WriteLine("missing key passed")
    end;
  end Main;
end Program;
