//! case: s28_3_0096
//! doc: spec
//! section: 28.3
//! title: under the trust-admission report mode, awaiting the task in its sources' scope reports nothing
//! rule: The settlement obligation of a frame holding a borrowed receiver is
//!   discharged by awaiting the task before any source's scope exits (spec
//!   §30.1); the trust-admission report mode reports only admissions that
//!   rest on a K4.3-recorded trust clause, and an awaited frame carries none.
//! cites: 28.3, 30.1
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 4605
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
  public static async method Fire(c: Conn): Task
    await c.PingAsync();
  end Fire;
end Program;
