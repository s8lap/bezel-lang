//! case: s28_3_0024
//! doc: spec
//! section: 28.3
//! title: a using var local can never be moved out of
//! rule: `using var` is never movable out of: `using var fresh := new Buffer(); return
//!   fresh;` is a compile error.
//! expect: reject
//! diagnostic: using var
module Conf;

public class Buffer
  public constructor()
  end constructor;

  public method CloneBuffer(): owned Buffer => new Buffer();
end Buffer;

public class Wrapper
  private owned _inner: Buffer;

  public constructor(owned inner: Buffer)
    _inner := inner;
  end constructor;
end Wrapper;

public class Program
  public method Wrap(s: Buffer): owned Wrapper
    using var copy := s.CloneBuffer();
    return new Wrapper(move copy);
  end Wrap;
end Program;
