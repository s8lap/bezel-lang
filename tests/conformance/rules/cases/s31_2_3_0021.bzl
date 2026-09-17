//! case: s31_2_3_0021
//! doc: spec
//! section: 31.2.3
//! title: a non-exhaustive catch block draws no diagnostic
//! rule: A `catch` block need not be exhaustive. Exceptions not matched by any arm
//!   propagate to the enclosing handler. The compiler does not warn on non-exhaustive
//!   `catch` blocks — non-exhaustiveness is the normal case.
//! cites: 31.2.3, 31.2, 19.6
//! expect: quiet
module Conf;

public class Worker
  public method Run(text: string)
    try
      Console.WriteLine(text);
    catch
      var ex: FormatException => Console.WriteLine(ex.Message)
    end try;
  end Run;
end Worker;
