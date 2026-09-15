//! case: l6_6_0002
//! doc: stdlib
//! section: 6.6
//! title: OwningTaskCompletionSource<T> is the admitted spelling for a class T
//! rule: For class T. Result is transferred via `owned` parameter; produced
//!   task is Pattern B. OwningTaskCompletionSource<T> where T: class.
//! cites: 6.6
//! expect: accept
module Conf;

import System.Threading;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Make()
    using var tcs := new OwningTaskCompletionSource<Widget>();
  end Make;
end Program;
