//! case: s30_3_0016
//! doc: spec
//! section: 30.3
//! title: Wait on an OwningTask called with an argument is rejected
//! rule: public method Wait();
//! cites: 30.3, 26.5, 22.6
//! expect: reject
//! diagnostic: Wait
module Conf;

public class R
  public constructor()
  end constructor;
end R;

public class Program
  public static method Main(): int
    using var t := Task.FromOwnedResult(new R());
    t.Wait(3);
    return 0;
  end Main;
end Program;
