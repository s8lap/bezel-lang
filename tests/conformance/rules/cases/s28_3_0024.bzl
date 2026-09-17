//! case: s28_3_0024
//! doc: spec
//! section: 28.3
//! title: a using var local can never be moved out of
//! rule: `using var` is never movable out of: `using var fresh := new Buffer(); return
//!   fresh;` is a compile error; a factory uses plain `var`.
//! cites: 28.10
//! expect: reject
//! diagnostic: using var
module Conf;

public class Buffer
  public constructor()
  end constructor;
end Buffer;

public class Factory
  public method Make(): owned Buffer
    using var fresh := new Buffer();
    return fresh;
  end Make;
end Factory;
