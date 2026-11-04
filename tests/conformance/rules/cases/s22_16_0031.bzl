//! case: s22_16_0031
//! doc: spec
//! section: 22.16
//! title: a contract clause is checked under the pure-body rules: an unmarked call is rejected
//! rule: The expression must be of type `bool` ([§4.1](#sec-bool) — no
//!   truthiness), and is admitted under the pure-body rules of
//!   [§22.15](#sec-pure-body-rules), applied expression-wise
//! cites: 22.16, 22.15
//! expect: reject
//! diagnostic: pure
//! issue: 7453
module Conf;

public class Ops
  public method Check(sb: StringBuilder): bool
    requires(sb.Length > 0)

    return true;
  end Check;
end Ops;
