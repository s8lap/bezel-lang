//! case: s32_12_0003
//! doc: spec
//! section: 32.12
//! title: Ptr<T> instantiated at a class type is rejected
//! rule: The pointee gate (per-instantiation): `T` is drawn from the FFI-pointee set; a
//!   class type is not a member of it.
//! cites: 32.12
//! expect: reject
//! diagnostic: pointee
module Conf;
import System.Interop;

public class Widget
  public constructor()
  end constructor;
end Widget;

public static class Memory
  public static method Peek(p: Ptr<Widget>): int
    return 0;
  end Peek;
end Memory;
