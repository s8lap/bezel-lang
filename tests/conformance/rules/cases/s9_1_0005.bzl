//! case: s9_1_0005
//! doc: spec
//! section: 9.1
//! title: comparing a positional record with an inadmissible field type is rejected at the call site
//! rule: A positional record with an inadmissible field declares fine, but
//!   comparing it is a compile error rather than silent reference equality.
//! cites: 9.2, 9.3
//! expect: reject
//! diagnostic: admissible
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Wrapper(Conn: Connection);

public class Program
  public static method Compare(a: Wrapper, b: Wrapper): bool => a = b;
end Program;
