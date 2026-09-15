//! case: l3_1_0005
//! doc: stdlib
//! section: 3.1
//! title: AppendFormat raises FormatException on a hole index outside args
//! rule: AppendFormat's format string contains literal text and holes {n} —
//!   n a zero-based index into args; a hole index outside args raises
//!   FormatException.
//! cites: 3.1
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder();
    var args: Array<Object> := [1];
    try
      _ := sb.AppendFormat("{5}", args);
    catch
      FormatException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
