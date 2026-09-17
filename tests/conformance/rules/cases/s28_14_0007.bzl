//! case: s28_14_0007
//! doc: spec
//! section: 28.14
//! title: a call-bounded callee may not return the callback
//! rule: Within the callee the call-bounded callback may not be returned,
//!   captured by a closure that itself escapes the callee, or forwarded
//!   into a scoped parameter position.
//! cites: 28.14
//! expect: reject
//! diagnostic: return
module Conf;

public class Runner
  public method Echo(cb: () -> ()): () -> ()
    return cb;
  end Echo;
end Runner;
