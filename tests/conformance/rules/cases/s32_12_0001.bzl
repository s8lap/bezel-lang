//! case: s32_12_0001
//! doc: spec
//! section: 32.12
//! title: Ptr<T> members are usable over admitted pointee types
//! rule: `T` is drawn from the FFI-pointee set: integer types, `float`, `double`,
//!   `bool`, `IntPtr`, `UIntPtr`, and C-compatible records.
//! cites: 32.12
//! expect: accept
module Conf;
import System.Interop;

public static class Memory
  public static method Peek(p: Ptr<int>): int
    return p.Load();
  end Peek;

  public static method PeekScaled(p: Ptr<double>, ids: Ptr<IntPtr>): double
    var q := p.Add(1);
    var addr := ids.LoadAt(0);
    return q.Load();
  end PeekScaled;
end Memory;
