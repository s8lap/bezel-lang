//! case: s30_3_0021
//! doc: spec
//! section: 30.3
//! title: Wait called with an argument on a spawned task is rejected
//! rule: public method Wait();
//! cites: 30.3, 26.5, 22.6
//! expect: reject
//! diagnostic: Wait
module Conf;

public class Program
  public static method Main(): int
    using var d := Domain.Spawn(lambda() => 5);
    d.Wait(1);
    return 0;
  end Main;
end Program;
