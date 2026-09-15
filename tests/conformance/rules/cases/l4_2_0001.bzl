//! case: l4_2_0001
//! doc: stdlib
//! section: 4.2
//! title: IOwningCollection<T>.Add takes owned and transfers ownership in
//! rule: IOwningCollection<T> declares method Add(owned item: T); the
//!   mutating methods of the owning family take owned parameters and take
//!   ownership of the new element.
//! cites: 4.2, 4.5, 28.11
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public method FillOwning<T>(target: IOwningCollection<T>, owned item: T) where T: class
  target.Add(move item);
end FillOwning;

public class Program
  public method Demo()
    using var pool := new OwningArray<Widget>();
    FillOwning(pool, new Widget());
  end Demo;
end Program;
