//! case: s23_1_0007
//! doc: spec
//! section: 23.1
//! title: assigning Task<int>.Result outside a constructor is rejected
//! rule: A `readonly` auto-property may be assigned only in a field initializer or in a constructor
//! cites: 30.3
//! expect: reject
//! diagnostic: readonly
module Conf;

public class Program
  public static method Main(): int
    using var t := Task.FromResult(1);
    t.Result := 3;
    return 0;
  end Main;
end Program;
