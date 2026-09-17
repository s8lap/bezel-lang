//! case: s1_3_0005
//! doc: spec
//! section: 1.3
//! title: an identifier merely containing an underscore is ordinary
//! rule: Identifiers merely containing underscores (`_camelCase`, `snake_case`,
//!   `_f`) are ordinary identifiers; only the bare single-character token is
//!   reserved.
//! expect: accept
module Conf;

public class Holder
  private method Use()
    var _x := 5;
  end Use;
end Holder;
