//! case: s29_6_0002
//! doc: spec
//! section: 29.6
//! title: yield return inside a try block with only a finally is admitted
//! rule: `yield return` or `yield break` inside a `try` block whose
//!   surrounding statement has only a `finally`: allowed. `using var`
//!   compiles to `try`/`finally` (no `catch`), so it is the preferred
//!   resource-management mechanism inside iterators.
//! cites: 29.6
//! expect: accept
module Conf;

public class Program
  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
    finally
      Console.WriteLine("cleanup");
    end try;
  end Gen;
end Program;
