//! case: s9_1_0006
//! doc: spec
//! section: 9.1
//! title: comparing a positional record with an admissible field type is accepted
//! rule: Value equality is generated when every participating positional field is
//!   of an admissible component type; comparing such a record is unrestricted.
//! cites: 9.2, 9.3
//! expect: accept
module Conf;

public record Wrapper(Conn: int);

public class Program
  public static method Compare(a: Wrapper, b: Wrapper): bool => a = b;
end Program;
