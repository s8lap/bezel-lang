//! case: s17_11_0024
//! doc: spec
//! section: 17.11
//! title: the same default(T) field is admitted where the type is named at a nullable class
//! rule: so at every admitted substitution the result is that type's zero
//!   value, and `null` exactly at a nullable substitution ... the result may
//!   initialize or be stored into any `T`-typed location.
//! cites: 17.11, 3.6
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Cell<T>
  public V: T := default(T);

  public constructor()
  end constructor;
end Cell;

public class User
  public static method Run(c: Cell<Widget?>): int => 1;
end User;
