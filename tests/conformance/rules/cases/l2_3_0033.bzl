//! case: l2_3_0033
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToInt32 out of range raises OverflowException, which an IFailure arm passes
//! rule: The `To*` methods raise `FormatException` when the string is not a
//!   well-formed representation of the target type and `OverflowException` when
//!   the value does not fit — the same contract as the §9 per-type `Parse`
//!   family these conversions mirror … A class whose condition can come from
//!   the program itself, or from data whose supplier the library cannot see, is
//!   not marked, and makes no statement: text that does not parse and a
//!   document that does not deserialize may be a user's typing or the program's
//!   own constant (`FormatException`, `SerializationException`); a condition
//!   about an argument, the state of an object the program holds or the
//!   program's arithmetic can be the program's (`ArgumentException`,
//!   `InvalidOperationException`, `KeyNotFoundException`, `OverflowException`).
//! cites: 2.3, 1
//! expect: run
//! stdout: out of range passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    try
      try
        _ := Convert.ToInt32("99999999999");
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      OverflowException => Console.WriteLine("out of range passed")
    end;
  end Main;
end Program;
