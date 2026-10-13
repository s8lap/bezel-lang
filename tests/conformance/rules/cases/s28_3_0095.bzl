//! case: s28_3_0095
//! doc: spec
//! section: 28.3
//! title: under the trust-admission report mode, abandoning a task minted over a borrowed receiver reports
//! rule: In that mode, at every site where the compiler makes an admission
//!   that rests on a K4.3-recorded trust clause, it **emits a warning** ...
//!   the **frame-held** clause (§30.1, §30.5) ... **The warning** names the
//!   rule and its clause ... The site is the admitting site — the store, the
//!   call, the abandon or the `await` the clause admits
//! cites: 28.3, 30.1, 30.10
//! expect: warn
//! diagnostic: trust
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
  public static method Fire(c: Conn)
    Task.FireAndForget(c.PingAsync());
  end Fire;
end Program;
