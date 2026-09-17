//! case: s28_3_0038
//! doc: spec
//! section: 28.3
//! title: a static method returning owned T announces its classification and is admitted
//! rule: Every admissible static signature is type-announced: `owned` on the declared
//!   return type admits a fresh or owned value.
//! cites: C.3
//! expect: accept
module Conf;

public class Resource
  public constructor()
  end constructor;
end Resource;

public class Picker
  public static method Pick(a: Resource, b: Resource): owned Resource => new Resource();
end Picker;
