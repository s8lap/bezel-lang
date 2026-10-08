//! case: s9_7_0030
//! doc: spec
//! section: 9.7
//! title: an IReadOnlySpan parameter is read as holding borrows and may not be an async parameter
//! rule: every rule of this specification that reads a value, parameter, local or
//!   substitution that holds borrows, or whose type is a value type that
//!   holds borrows, reads one declared at that interface type as it reads the
//!   span, by its declared type, nullable or not
//! cites: 9.7, 30.1
//! expect: reject
//! diagnostic: async
//! issue: 5659
module Conf;

public class Client
  public async method Scan(s: IReadOnlySpan<int>): Task
  end Scan;
end Client;
