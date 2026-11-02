//! case: s30_3_0020
//! doc: spec
//! section: 30.3
//! title: Wait called with an argument through a null-conditional chain is rejected
//! rule: public method Wait();
//! cites: 30.3, 26.5, 22.6, 7.5
//! expect: reject
//! diagnostic: Wait
module Conf;

public class Holder
  public Cell: Task<int>? := null;
end Holder;

public class Program
  public static method Main(): int
    var h: Holder? := new Holder();
    h?.Cell?.Wait(3);
    return 0;
  end Main;
end Program;
