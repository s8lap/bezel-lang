//! case: s22_2_0002
//! doc: spec
//! section: 22.2
//! title: a method with no declared return type may use an assignment as its arrow body
//! rule: A method with **no declared return type** may also use a single
//!   *assignment* as its arrow body, the same shape the accessor form
//!   `set => _x := value;` has always had.
//! cites: 22.2
//! expect: accept
module Conf;

public class Order
  private _total: int := 0;

  public constructor()
  end constructor;

  public method SetTotal(v: int) => _total := v;
end Order;
