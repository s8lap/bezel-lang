//! case: s23_1_0006
//! doc: spec
//! section: 23.1
//! title: assigning OwningTask<T>.Result outside a constructor is rejected
//! rule: A `readonly` auto-property may be assigned only in a field initializer or in a constructor
//! cites: 30.3
//! expect: reject
//! diagnostic: readonly
module Conf;

public class K
  public constructor() end constructor;
end K;

public class Program
  public static async method P(): OwningTask<K>
    return new K();
  end P;

  public static method Main(): int
    using var t := P();
    t.Result := new K();
    return 0;
  end Main;
end Program;
