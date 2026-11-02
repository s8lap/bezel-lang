//! case: s30_3_0023
//! doc: spec
//! section: 30.3
//! title: Wait called with an argument on an unbound task temporary is rejected
//! rule: public method Wait();
//! cites: 30.3, 26.5, 22.6
//! expect: reject
//! diagnostic: Wait
module Conf;

public class Program
  public static async method F(): Task<int>
    return 1;
  end F;

  public static method Main(): int
    F().Wait(1);
    return 0;
  end Main;
end Program;
