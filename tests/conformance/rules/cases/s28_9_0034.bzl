//! case: s28_9_0034
//! doc: spec
//! section: 28.9
//! title: a Type value is runtime-managed and its holder carries no obligation
//! rule: `Type` is runtime-managed: the runtime holds exactly one instance per
//!   type for the life of the process and user code never allocates one, so no
//!   `Type` expression is an owned production, none lends a borrow, and no
//!   holder carries an obligation for one — the compiler neither warns about
//!   an undisposed one nor requires it to appear in a `using var`.
//! cites: 28.9, 17.10
//! expect: quiet
//! issue: 4393
module Conf;

public class Program
  public method Describe(): string
    var t := typeof(Program);
    return t.Name;
  end Describe;
end Program;
