//! case: s32_8_0017
//! doc: spec
//! section: 32.8
//! title: Interop.CodeAddress rejects a bound instance-method reference
//! rule: **The argument position carries this section's static admission rule** —
//!   static method reference or non-capturing lambda, checked exactly as at a plain
//!   extern callback parameter; a capture-bearing form is rejected with the same
//!   diagnostic (the `env` clause does not apply: there is no bounding call).
//! cites: 32.8
//! expect: reject
//! diagnostic: environment
//! notes: l10_6_0001 is the static-reference twin; l10_6_0002 covers the capturing
//!   lambda, this case the other capture-bearing form (the receiver is the capture).
module Conf;
import System.Interop;

public class Handler
  public constructor()
  end constructor;

  public method OnSignal(sig: int)
  end OnSignal;

  public method Address(): IntPtr
    return Interop.CodeAddress<(int) -> ()>(this.OnSignal);
  end Address;
end Handler;
