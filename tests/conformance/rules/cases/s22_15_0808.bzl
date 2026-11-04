//! case: s22_15_0808
//! doc: spec
//! section: 22.15
//! title: a pure constructor whose base constructor is not pure is rejected
//! rule: A pure constructor's `base(…)`/`this(…)` chain target must itself be pure.
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
//! issue: 8134
//! notes: no pure method is involved: the declaration itself is rejected. Twin s22_15_0801 chains to the built-in pure (message) form.
module Conf;

public class BaseError: Exception
  public constructor(message: string) base(message);
    Console.WriteLine("side effect");
  end constructor;
end BaseError;

public class MyError: BaseError
  public pure constructor(message: string) base(message);
  end constructor;
end MyError;

public class Program
  public static method Main()
    Console.WriteLine("x");
  end Main;
end Program;
