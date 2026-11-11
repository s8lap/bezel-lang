//! case: l9_2_0020
//! doc: stdlib
//! section: 9.2
//! title: double.Parse out of range raises OverflowException, which an IFailure arm passes
//! rule: raises `OverflowException`: the type cannot represent the number the
//!   input spells … A class whose condition can come from the program itself,
//!   or from data whose supplier the library cannot see, is not marked, and
//!   makes no statement: text that does not parse and a document that does not
//!   deserialize may be a user's typing or the program's own constant
//!   (`FormatException`, `SerializationException`); an argument, the state of
//!   an object the program holds and the program's arithmetic are the program's
//!   (`ArgumentException`, `InvalidOperationException`, `KeyNotFoundException`,
//!   `OverflowException`).
//! cites: 9.2, 1
//! expect: run
//! stdout: out of range passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    try
      try
        _ := double.Parse("1e999");
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      OverflowException => Console.WriteLine("out of range passed")
    end;
  end Main;
end Program;
