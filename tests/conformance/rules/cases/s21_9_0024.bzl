//! case: s21_9_0024
//! doc: spec
//! section: 21.9
//! title: a unit-returning call as a statement draws no unused-result warning
//! rule: Method calls returning `()` ... are the common cases; no warning is
//!   emitted for these.
//! cites: 21.9
//! expect: quiet
module Conf;

public class Writer
  public method Emit(text: string)
    Console.WriteLine(text);
  end Emit;
end Writer;
