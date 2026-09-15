//! case: s21_1_0003
//! doc: spec
//! section: 21.1
//! title: a runtime const local may not be initialized from an owned production
//! rule: The initializer of a runtime `const` must not be an owned
//!   production — rejected, with a diagnostic naming this rule and the
//!   `using var` remedy: a fresh `new`.
//! cites: 21.1
//! expect: reject
//! diagnostic: owned
module Conf;

public class Resource
  public constructor()
  end constructor;
end Resource;

public class Program
  public static method Main()
    const cfg := new Resource();
  end Main;
end Program;
