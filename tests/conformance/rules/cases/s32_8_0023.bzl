//! case: s32_8_0023
//! doc: spec
//! section: 32.8
//! title: an unpaired callback parameter beside an env pair still rejects a capturing lambda
//! rule: **Unpaired function-typed parameters are unchanged** — the static admission
//!   above governs them.
//! cites: 32.8
//! expect: reject
//! diagnostic: environment
//! notes: the capturing lambda goes to `done`, the declaration's unpaired callback;
//!   valid twin passes the static `Scanner.OnDone` there instead.
module Conf;

extern "libexample"
  public method scan_rows(handle: IntPtr, cb: (env, int) -> int, env,
      done: (int) -> ()): int;
end;

public static class Scanner
  public static method OnDone(code: int)
  end OnDone;

  public static method CountAbove(handle: IntPtr, threshold: int): int
    return scan_rows(handle,
        lambda(code: int): int => int(code > threshold),
        lambda(code: int) => do
          if code > threshold then
            Scanner.OnDone(code);
          end;
        end);
  end CountAbove;
end Scanner;
