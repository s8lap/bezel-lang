//! case: s2_2_0002
//! doc: spec
//! section: 2.2
//! title: naming a type requires importing its declaring module
//! rule: All other library modules require explicit `import` — importing
//!   `System.Threading` is required to name `CancellationTokenSource`.
//! cites: 2.2, 2.3
//! expect: reject
//! diagnostic: import
module Conf;

public class Holder
  private method Use()
    using var cts := new CancellationTokenSource();
  end Use;
end Holder;
