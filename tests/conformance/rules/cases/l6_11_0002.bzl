//! case: l6_11_0002
//! doc: stdlib
//! section: 6.11
//! title: an owning collection of sendable elements is an admitted Locked<T> interior
//! rule: owning collections the §4 statement makes sendable (`OwningSet<T>`
//!   at a sendable `T`, `OwningDictionary<K, V>` at a sendable `V` and a
//!   sendable-value or `string` key, …) ... qualify ...
//!   Locked<OwningDictionary<string, Session>> is the admitted spelling.
//! cites: 6.11, 30.12
//! expect: accept
module Conf;

import System.Threading;

public sealed class Session
  public Name: string;
  public constructor(name: string) Name := name; end constructor;
end Session;

public class Program
  public method Make()
    using var lk := new Locked<OwningDictionary<string, Session>>(new OwningDictionary<string, Session>());
  end Make;
end Program;
