//! case: s28_3_0095
//! doc: spec
//! section: 28.3
//! title: under the trust-admission report mode, abandoning a task minted over a borrowed receiver reports
//! rule: In the trust-admission report mode, a borrow held by a pending frame
//!   past the call that minted it — a task passed to Task.FireAndForget —
//!   is an admission resting on a K4.3-recorded trust clause (spec §30.1's
//!   settlement obligation), and the compiler emits a warning naming the rule
//!   and its clause.
//! cites: 28.3, 30.1, 30.10
//! expect: warn
//! diagnostic: trust
//! flags: --report-trust-admissions
module Conf;

import System.Threading;

public class Conn
  public constructor()
  end constructor;

  public async method PingAsync(): Task
    await Task.Delay(1);
  end PingAsync;
end Conn;

public class Program
  public static method Fire(c: Conn)
    Task.FireAndForget(c.PingAsync());
  end Fire;
end Program;
