//! case: s2_2_0004
//! doc: spec
//! section: 2.2
//! title: a duplicate plain import of the same module is idempotent
//! rule: A duplicate plain `import` of a module already imported in the same
//!   file is idempotent: the later occurrence adds nothing and is not
//!   diagnosed.
//! expect: accept
module Conf;
import System.Threading;
import System.Threading;

public class Holder
  private method Use()
    using var cts := new CancellationTokenSource();
  end Use;
end Holder;
