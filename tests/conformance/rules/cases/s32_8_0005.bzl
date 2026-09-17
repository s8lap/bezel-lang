//! case: s32_8_0005
//! doc: spec
//! section: 32.8
//! title: a bound instance-method reference is admitted at an env-paired callback parameter
//! rule: The pair makes the callback parameter capture-admitting: any function value --
//!   including a capturing lambda and a bound instance-method reference
//!   `this.OnRow` -- may be passed there.
//! cites: 32.8
//! expect: accept
module Conf;
import System.Interop;

extern "libexample"
  method scan_rows(handle: IntPtr, sql: CString,
      cb: (env, int) -> int, env): int;
end;

public class RowCounter
  private _rows: int := 0;

  public constructor()
  end constructor;

  private method OnRow(code: int): int
    _rows := _rows + 1;
    return 0;
  end OnRow;

  public method Count(handle: IntPtr, sql: CString): int
    return scan_rows(handle, sql, this.OnRow);
  end Count;
end RowCounter;
