//! case: l6_6_0001
//! doc: stdlib
//! section: 6.6
//! title: instantiating TaskCompletionSource<T> with a class T is rejected
//! rule: For non-class T (copyable value types, string, runtime-managed
//!   reference types). Instantiating with a class T is a compile error
//!   pointing to OwningTaskCompletionSource<T>.
//! cites: 6.6
//! expect: reject
//! diagnostic: OwningTaskCompletionSource
module Conf;

import System.Threading;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Make()
    using var tcs := new TaskCompletionSource<Widget>();
  end Make;
end Program;
