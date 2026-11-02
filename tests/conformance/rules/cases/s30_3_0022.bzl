//! case: s30_3_0022
//! doc: spec
//! section: 30.3
//! title: Wait called with a named argument is rejected
//! rule: public method Wait();
//! cites: 30.3, 26.5, 22.6, 22.5
//! expect: reject
//! diagnostic: Wait
module Conf;

public class Program
  public static method Main(): int
    using var t := Task.FromResult(2);
    t.Wait(x := 1);
    return 0;
  end Main;
end Program;
