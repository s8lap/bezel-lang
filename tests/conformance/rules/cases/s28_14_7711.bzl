//! case: s28_14_7711
//! doc: spec
//! section: 28.14
//! title: a call-bounded function parameter may not be returned
//! rule: returned, captured by a closure that itself escapes the callee
//! cites: 28.3
//! expect: reject
//! diagnostic: call-bounded
module Conf;
public class C
  public static method F(g: () -> int): () -> int
    return g;
  end F;
end C;
