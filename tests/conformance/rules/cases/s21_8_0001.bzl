//! case: s21_8_0001
//! doc: spec
//! section: 21.8
//! title: throw expression as a statement raises and is caught
//! rule: "`throw expression;` and the bare re-raise `throw;` are the
//!   statement spellings of the `throw` expression, equivalent to the
//!   expression form followed by `;`."
//! cites: 21.8, 31.1
//! expect: run
//! stdout: bad value
module Conf;

public class Program
  public static method Main()
    try
      throw new ArgumentException("bad value");
    catch
      var ex: ArgumentException => Console.WriteLine(ex.Message);
    end try;
  end Main;
end Program;
