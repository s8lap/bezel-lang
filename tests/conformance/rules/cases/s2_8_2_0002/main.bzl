//! case: s2_8_2_0002
//! doc: spec
//! section: 2.8.2
//! title: purity carries across the module boundary into a contract clause
//! rule: The artifact interface carries the purity modifier of every member
//!   it describes; a `requires` clause may call only `pure` members (§22.16),
//!   and that obligation is checkable against an imported member exactly as
//!   against a local one.
//! cites: 2.8.2, 22.16
//! expect: accept
module App;
import Lib;

public class Box
  private _n: int;

  public constructor()
    _n := 0;
  end constructor;

  public method SetN(n: int)
    requires(MathUtil.Square(n) >= 0)

    _n := n;
  end SetN;

  public method N(): int => _n;
end Box;
