//! case: s27_5_0007
//! doc: spec
//! section: 27.5
//! title: a closure may not be returned from the method while it captures a borrow
//! rule: While a closure captures anything, it may not be returned from the
//!   method — a function type announces no source set, so no caller could
//!   bound it (§28.3, v0.298).
//! cites: 27.5, 28.3
//! expect: reject
//! diagnostic: returned
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Greet()
    Console.WriteLine("hi");
  end Greet;
end Widget;

public class Program
  public method Make(w: Widget): () -> ()
    return lambda() => w.Greet();
  end Make;
end Program;
