//! case: s21_9_0021
//! doc: spec
//! section: 21.9
//! title: a discarded string-returning call on a fluent receiver draws the unused-result warning
//! rule: An expression statement whose result is a non-`()` type is permitted but
//!   the compiler emits a warning: the result is silently discarded, which may
//!   indicate a bug.
//! cites: 21.9
//! expect: warn
//! diagnostic: discarded
module Conf;

public class Writer
  public method Emit(text: string)
    using var sb := new StringBuilder(text);
    sb.ToString();
  end Emit;
end Writer;
