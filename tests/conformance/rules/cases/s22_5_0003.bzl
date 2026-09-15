//! case: s22_5_0003
//! doc: spec
//! section: 22.5
//! title: a parameter bound both positionally and by name is rejected
//! rule: Arity filtering and named-argument-to-parameter binding
//!   (§22.5) resolve each supplied argument to exactly one parameter; a
//!   parameter named again after it was already bound positionally has no
//!   consistent binding.
//! cites: 22.5, 22.6
//! expect: reject
//! diagnostic: duplicate
module Conf;

public class Logger
  public static method Log(message: string, level: string := "INFO")
  end Log;
end Logger;

public class Program
  public static method Main()
    Logger.Log("Failed", message := "Retry");
  end Main;
end Program;
