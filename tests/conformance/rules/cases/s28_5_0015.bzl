//! case: s28_5_0015
//! doc: spec
//! section: 28.5
//! title: a generic owned field instantiated at a class type is admitted
//! rule: A generic class's `owned` field is checked per instantiation; a
//!   class-typed instantiation of `where T: class` is admissible.
//! cites: 14.2, 28.5
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Box<T> where T: class
  private owned _slot: T;

  public constructor(owned slot: T)
    _slot := slot;
  end constructor;
end Box;

public class Program
  public method Demo()
    var b: Box<Widget>;
  end Demo;
end Program;
