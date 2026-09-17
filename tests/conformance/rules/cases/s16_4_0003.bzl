//! case: s16_4_0003
//! doc: spec
//! section: 16.4
//! title: step may not be applied to an open-ended range
//! rule: "`step` may not be applied to open-ended ranges."
//! cites: 16.4
//! expect: reject
//! diagnostic: open-ended
module Conf;

public class Program
  public static method Main()
    var r := 10.. step 1;
  end Main;
end Program;
