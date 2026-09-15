//! case: l6_11_0001
//! doc: stdlib
//! section: 6.11
//! title: a borrowing collection of class elements cannot be a Locked<T> interior
//! rule: borrowing collections of class elements do not (Locked<Dictionary<string,
//!   Session>> is rejected — own the values: Locked<OwningDictionary<string,
//!   Session>>).
//! cites: 6.11, 30.12
//! expect: reject
//! diagnostic: sendable
module Conf;

import System.Threading;

public sealed class Session
  public Name: string;
  public constructor(name: string) Name := name; end constructor;
end Session;

public class Program
  public method Make()
    using var lk := new Locked<Dictionary<string, Session>>(new Dictionary<string, Session>());
  end Make;
end Program;
