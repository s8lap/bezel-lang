//! case: s28_6_0016
//! doc: spec
//! section: 28.6
//! title: disposing a caught exception binding is rejected
//! rule: The borrow receivers for `Dispose()` include a `catch`-arm binding.
//! cites: 28.6, 31.1
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Program
  public method Attempt()
    try
      throw new InvalidOperationException("boom");
    catch
      var ex: InvalidOperationException => ex.Dispose()
    end try;
  end Attempt;
end Program;
