//! case: s29_11_0031
//! doc: spec
//! section: 29.11
//! title: a bound lazy query with no consumption point draws the must-use warning
//! rule: A bound lazy query whose enumeration right is never spent draws
//!   K3.14's must-use warning at its auto-drop (v0.301); the warning is
//!   suppressed where the binding's consumption point is statically visible.
//!   §28.9's ceremony exemption does not reach a compiler-synthesized frame,
//!   so the lint applies on §28.3's ordinary terms.
//! cites: 29.11, 28.3, 28.9
//! expect: warn
//! diagnostic: must use
module Conf;

public class Scanner
  public method Scan(xs: Array<int>)
    var q := xs.Where(lambda(x) => x > 0);
  end Scan;
end Scanner;
