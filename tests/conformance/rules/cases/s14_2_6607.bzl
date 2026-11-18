//! case: s14_2_6607
//! doc: spec
//! section: 14.2
//! title: stdlib record `TimeSpan` does not satisfy `where T: class` in a field type
//! rule: Non-nullable reference types only — classes (including `Array<T>`, `OwningArray<T>`, and other heap collection types), interfaces, `string`, function types.
//! cites: 14.2
//! expect: reject
//! diagnostic: class
//! issue: 8591
module Conf;
public class Box<T> where T: class
  public constructor()
  end constructor;
end Box;
public class Holder
  private _b: Box<TimeSpan>?;

  public constructor()
  end constructor;
end Holder;
