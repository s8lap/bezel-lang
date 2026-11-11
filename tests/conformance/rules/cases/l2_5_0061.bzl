//! case: l2_5_0061
//! doc: stdlib
//! section: 2.5
//! title: a DateTime range overflow passes an IFailure arm
//! rule: a band-1 condition of the arithmetic domain (§1), raised with the
//!   arithmetic class and not with an argument-blaming one … A class whose
//!   condition can come from the program itself, or from data whose supplier
//!   the library cannot see, is not marked, and makes no statement: text that
//!   does not parse and a document that does not deserialize may be a user's
//!   typing or the program's own constant (`FormatException`,
//!   `SerializationException`); a condition about an argument, the state of an
//!   object the program holds or the program's arithmetic can be the program's
//!   (`ArgumentException`, `InvalidOperationException`, `KeyNotFoundException`,
//!   `OverflowException`).
//! cites: 2.5, 1
//! expect: run
//! stdout: overflow passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    try
      try
        _ := DateTime.MaxValue.AddDays(1.0);
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      OverflowException => Console.WriteLine("overflow passed")
    end;
  end Main;
end Program;
