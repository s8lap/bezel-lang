//! case: s31_2_1_0004
//! doc: spec
//! section: 31.2.1
//! title: a binding over unrelated types types at Exception, not either listed type
//! rule: For alternatives sharing nothing more specific, the nearest common
//!   base is `Exception`; every listed type is `Exception`-derived, so the
//!   binding's static type is never narrower than that.
//! cites: 31.2.1
//! expect: reject
//! diagnostic: convert
//! issue: 4644
module Conf;

public class SiblingA: Exception
  public constructor(message: string) base(message);
  end constructor;
end SiblingA;

public class SiblingB: Exception
  public constructor(message: string) base(message);
  end constructor;
end SiblingB;

public class Program
  public static method Main()
    try
      throw new SiblingB("boom");
    catch
      var ex: SiblingA | SiblingB => do
        var narrow: SiblingA := ex;
      end,
    end try;
  end Main;
end Program;
