//! case: s28_3_0002
//! doc: spec
//! section: 28.3
//! title: an undischarged plain-var owned local raises the must-use warning
//! rule: An owned value reaching its scope's end undischarged is disposed there
//!   automatically; the only difference between the normal and the exception edge
//!   is a lint — a plain-`var` implicit drop on normal exit raises the must-use
//!   warning, and the exception edge is silent.
//! cites: 28.3, C.3
//! expect: warn
//! diagnostic: must use
module Conf;

public class Resource
  public constructor()
  end constructor;
end Resource;

public class Program
  public static method Main()
    var r := new Resource();
  end Main;
end Program;
