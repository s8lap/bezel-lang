//! case: s22_12_0043
//! doc: spec
//! section: 22.12
//! title: a static owned readonly CancellationTokenSource is silent under 4(a)
//! rule: The four members outside rule 2's form (i) — `Locked<T>`/`RwLocked<T>`,
//!   `FrozenArray<T>`, and `CancellationTokenSource`, the process-global shutdown
//!   source — are in the warning-suppression set; `owned` enrols the field in
//!   §22.13's program-exit disposal.
//! cites: 22.12, 22.13
//! expect: quiet
module Conf;

import System.Threading;

public static class Shutdown
  private static owned readonly _source: CancellationTokenSource :=
      new CancellationTokenSource();

  public static method Request()
    _source.Cancel();
  end Request;
end Shutdown;
