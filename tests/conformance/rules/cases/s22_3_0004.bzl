//! case: s22_3_0004
//! doc: spec
//! section: 22.3
//! title: an out parameter left unassigned on some normal-completion path is rejected
//! rule: The callee must assign every `out` parameter on every
//!   normal-completion (return) path; failing to do so is a compile error.
//! cites: 22.3
//! expect: reject
//! diagnostic: not assigned
module Conf;

public class Parser
  public static method TryParseFlag(s: string, out ok: bool): bool
    if s = "true" then
      ok := true;
      return true;
    else
      return false;
    end if;
  end TryParseFlag;
end Parser;
