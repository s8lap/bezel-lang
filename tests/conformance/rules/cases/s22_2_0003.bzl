//! case: s22_2_0003
//! doc: spec
//! section: 22.2
//! title: the assignment arrow body is an error under a declared return type
//! rule: On a method with a declared return type the assignment form is an
//!   error (an assignment produces no value).
//! cites: 22.2
//! expect: reject
//! diagnostic: produces no value
module Conf;

public class Order
  private _total: int := 0;

  public constructor()
  end constructor;

  public method SetTotal(v: int): int => _total := v;
end Order;
