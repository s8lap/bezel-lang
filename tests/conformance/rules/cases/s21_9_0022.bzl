//! case: s21_9_0022
//! doc: spec
//! section: 21.9
//! title: the fluent-receiver discard draws no unused-result warning
//! rule: One exemption: the fluent-receiver discard. A discarded method-call result
//!   draws no warning when it is a §28.3 borrow whose source set lies within the
//!   statement's receiver chain and whose static type equals the receiver's static
//!   type — the `return this` fluent shape (`sb.Append(x);`).
//! cites: 21.9, 28.3
//! expect: quiet
module Conf;

public class Writer
  public method Emit(text: string)
    using var sb := new StringBuilder(text);
    sb.Append(text);
  end Emit;
end Writer;
