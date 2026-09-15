//! case: s17_7_0006
//! doc: spec
//! section: 17.7
//! title: the new() shorthand is admitted at a default parameter value
//! rule: "The written-declared-type positions ... (iii) a default parameter
//!   value ... The shorthand is permitted at the written-declared-type
//!   positions (i), (ii), (iii) ..." (v0.296)
//! cites: 17.7
//! expect: accept
module Conf;

public class Config
  public constructor()
  end constructor;
end Config;

public class Service
  public constructor()
  end constructor;

  public method Configure(owned cfg: Config := new())
    using var c := move cfg;
  end Configure;
end Service;
