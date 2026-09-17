//! case: s7_9_0004
//! doc: spec
//! section: 7.9
//! title: reading a non-defaultable local before it is assigned is rejected
//! rule: Definite-assignment analysis applies the same rule to locals, keyed
//!   on the same property: a local of **non-defaultable** type must be
//!   assigned before it is read. A non-nullable class type is the common
//!   non-defaultable case.
//! cites: 7.9, 17.11
//! expect: reject
//! diagnostic: assigned before it is read
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var w: Widget;
    var w2 := w;
  end Main;
end Program;
