//! case: l10_5_0002
//! doc: stdlib
//! section: 10.5
//! title: Ptr<Ptr<T>> nested pointer is not admitted
//! rule: Nested Ptr<Ptr<T>> is NOT admitted (language spec §32.12).
//! cites: 10.5
//! expect: reject
//! diagnostic: Ptr
module Conf;

import System.Interop;

public class Program
  public method Demo()
    var p: Ptr<Ptr<int>>;
  end Demo;
end Program;
