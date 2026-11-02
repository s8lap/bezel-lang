//! case: s30_3_0018
//! doc: spec
//! section: 30.3
//! title: Wait called with an argument on a parenthesized task is rejected
//! rule: public method Wait();
//! cites: 30.3, 26.5, 22.6
//! expect: reject
//! diagnostic: Wait
module Conf;

public class Program
  public static method Main(): int
    using var t := Task.FromResult(2);
    (t).Wait(3);
    return 0;
  end Main;
end Program;
