//! case: s20_0014
//! doc: spec
//! section: 20
//! title: two marked-move arms both meet to an owned result
//! rule: The smallest legal edit of s20_0013 — marking the second arm `move b` as
//!   well — makes both arms owned: "owned arms meet to owned ... the marked
//!   branch `move x` — `x` a local binding or `owned` parameter — transfers."
//! cites: 20, 28.3
//! expect: accept
module Conf;

public class Resource
  public constructor()
  end constructor;
end Resource;

public class Program
  private static method Compute()
    using var a := new Resource();
    using var b := new Resource();
    using var r := (if true then move a else move b);
  end Compute;
end Program;
