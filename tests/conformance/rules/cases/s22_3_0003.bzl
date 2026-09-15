//! case: s22_3_0003
//! doc: spec
//! section: 22.3
//! title: the callee assigns every out parameter on every normal-completion path
//! rule: The callee must assign every `out` parameter on every
//!   normal-completion (return) path.
//! cites: 22.3
//! expect: accept
module Conf;

public class Parser
  public static method TryParseFlag(s: string, out ok: bool): bool
    if s = "true" then
      ok := true;
      return true;
    else
      ok := false;
      return false;
    end if;
  end TryParseFlag;
end Parser;
