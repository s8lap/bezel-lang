//! case: s4_8_0001
//! doc: spec
//! section: 4.8
//! title: IntPtr is available without an explicit import
//! rule: IntPtr and UIntPtr are compiler intrinsics ... These types live in
//!   the auto-imported System module.
//! cites: 4.8
//! expect: accept
module Conf;

public class Holder
  private _p: IntPtr := IntPtr.Zero;

  public constructor()
  end constructor;
end Holder;
