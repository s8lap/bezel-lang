//! case: l6_11_0007
//! doc: stdlib
//! section: 6.11
//! title: a static owned readonly Locked<T> registry compiles warning-free
//! rule: The canonical process-global registry — the static owned readonly
//!   idiom (rule-4 warning suppressed by §22.12 rule 4's
//!   warning-suppression set, the v0.186 ruling).
//! cites: 6.11, 22.12, 30.12
//! expect: quiet
module Conf;

import System.Threading;

public sealed class Session
  public Name: string;
  public constructor(name: string) Name := name; end constructor;
end Session;

public static class Sessions
  private static owned readonly _map: Locked<OwningDictionary<string, Session>>
      := new Locked<OwningDictionary<string, Session>>(new OwningDictionary<string, Session>());

  public static async method Touch(name: string): Task
    using var g := await _map.LockAsync();
    g.Value[name] := new Session(name);
  end Touch;
end Sessions;
