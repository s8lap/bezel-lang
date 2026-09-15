//! case: s2_3_0002
//! doc: spec
//! section: 2.3
//! title: a non-auto stdlib type is not visible without its import
//! rule: All other library modules require explicit `import`.
//!   `CancellationTokenSource` is NOT auto — it needs `import
//!   System.Threading;`.
//! cites: 2.3
//! expect: reject
//! diagnostic: CancellationTokenSource
module Conf;

public class Program
  public static method Main()
    using var src := new CancellationTokenSource();
  end Main;
end Program;
