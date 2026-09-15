//! case: s28_6_0017
//! doc: spec
//! section: 28.6
//! title: reading a caught exception binding without disposing it is admitted
//! rule: A `catch`-arm binding is a borrow; reading it (without disposing it)
//!   is ordinary borrow use.
//! cites: 28.6, 31.1
//! expect: accept
module Conf;

public class Program
  public method Attempt()
    try
      throw new InvalidOperationException("boom");
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message)
    end try;
  end Attempt;
end Program;
