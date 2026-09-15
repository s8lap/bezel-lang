//! case: s21_10_0005
//! doc: spec
//! section: 21.10
//! title: a class may not be declared as a local type
//! rule: "`class` and `interface` may not [be declared locally] — a class
//!   carries ownership, destructors, and the §28 disposal machinery, a
//!   body-scoped kernel surface for no recorded need."
//! cites: 21.10
//! expect: reject
//! diagnostic: class
module Conf;

public class Program
  public static method Main()
    class Helper
      public constructor()
      end constructor;
    end Helper;
    var h := new Helper();
    _ := h;
  end Main;
end Program;
