//! case: l10_6_0002
//! doc: stdlib
//! section: 10.6
//! title: Interop.CodeAddress rejects a capturing lambda argument
//! rule: a capture-bearing argument is rejected with §32.8's
//!   diagnostic; the env clause does not apply.
//! cites: 10.6
//! expect: reject
//! diagnostic: capture
module Conf;

import System.Interop;

public class Program
  public method Demo()
    var captured := 5;
    var addr := Interop.CodeAddress(lambda(x: int): int => x + captured);
  end Demo;
end Program;
