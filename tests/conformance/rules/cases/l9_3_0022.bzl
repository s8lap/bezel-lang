//! case: l9_3_0022
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse out of range raises OverflowException, which an IFailure arm passes
//! rule: a value outside `decimal`'s range … raises `OverflowException`. … A
//!   class whose condition can come from the program itself, or from data whose
//!   supplier the library cannot see, is not marked, and makes no statement:
//!   text that does not parse and a document that does not deserialize may be a
//!   user's typing or the program's own constant (`FormatException`,
//!   `SerializationException`); a condition about an argument, the state of an
//!   object the program holds or the program's arithmetic can be the program's
//!   (`ArgumentException`, `InvalidOperationException`, `KeyNotFoundException`,
//!   `OverflowException`).
//! cites: 9.3, 1
//! expect: run
//! stdout: out of range passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    try
      try
        _ := decimal.Parse("79228162514264337593543950336");
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      OverflowException => Console.WriteLine("out of range passed")
    end;
  end Main;
end Program;
