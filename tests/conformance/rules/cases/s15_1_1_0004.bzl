//! case: s15_1_1_0004
//! doc: spec
//! section: 15.1.1
//! title: an owning inline array of nullable slots defaults to all null
//! rule: Nullable owned slots default to all null, with no initializer
//!   required at declaration (§15.1.1).
//! cites: 15.1.1
//! expect: run
//! stdout: True
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Program
  public static method Main()
    var pool: [4 owned]Connection?;
    Console.WriteLine(pool[0] = null);
  end Main;
end Program;
