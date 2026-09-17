//! case: s27_1_0004
//! doc: spec
//! section: 27.1
//! title: the out-owned modifier compound forms its own function type
//! rule: Function types may include the modifier combination `out owned`;
//!   `method Foo(out owned r: T)` parallels `(out owned T) -> ()`.
//! cites: 27.1, 22.3
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  public static method Make(out owned w: Widget)
    w := new Widget();
  end Make;
end Factory;

public class Registry
  private _maker: (out owned Widget) -> () := Factory.Make;

  public constructor()
  end constructor;
end Registry;
