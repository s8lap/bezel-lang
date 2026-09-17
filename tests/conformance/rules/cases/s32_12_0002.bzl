//! case: s32_12_0002
//! doc: spec
//! section: 32.12
//! title: Ptr<Ptr<T>> is not admitted
//! rule: `Ptr<Ptr<U>>` is not admitted (v1 decline; `Ptr<IntPtr>` plus explicit
//!   `FromIntPtr` re-typing expresses every `T**` shape).
//! cites: 32.12
//! expect: reject
//! diagnostic: pointee
module Conf;
import System.Interop;

public static class Memory
  public static method Deref(pp: Ptr<Ptr<int>>): Ptr<int>
    return pp.Load();
  end Deref;
end Memory;
