//! case: s28_14_0003
//! doc: spec
//! section: 28.14
//! title: a scoped callback may not be returned from the callee
//! rule: The callee may invoke the callback or store it in a scoped field;
//!   it may not return it, move it into non-scoped storage, or store it in
//!   any other object.
//! cites: 28.14
//! expect: reject
//! diagnostic: return
module Conf;

public class Registry
  public method Echo(scoped cb: () -> ()): () -> ()
    return cb;
  end Echo;
end Registry;
