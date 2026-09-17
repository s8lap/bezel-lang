//! case: s17_4_0001
//! doc: spec
//! section: 17.4
//! title: not is is rejected — is not is a single two-token operator
//! rule: "`is not` is a single operator with two-token spelling — the parser
//!   does not accept `not is`."
//! cites: 17.4
//! expect: reject
//! diagnostic: is not
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Program
  public static method Check(a: Animal): bool
    var r := a not is Animal;
    return r;
  end Check;
end Program;
