//! case: s2_2_0003
//! doc: spec
//! section: 2.2
//! title: naming a type is admitted once its module is imported
//! rule: All other library modules require explicit `import`; once imported,
//!   naming the type is admitted.
//! cites: 2.2, 2.3
//! expect: accept
module Conf;
import System.Threading;

public class Holder
  private method Use()
    using var cts := new CancellationTokenSource();
  end Use;
end Holder;
