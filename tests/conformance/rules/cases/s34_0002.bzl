//! case: s34_0002
//! doc: spec
//! section: 34
//! title: an unrecognized documentation tag is preserved, not rejected
//! rule: Unrecognized tags are preserved verbatim in the same metadata:
//!   recognition affects only how documentation tooling may render a tag,
//!   never compilation.
//! expect: accept
module Conf;

public class Widget
  /// <customNote>internal tooling hint, not in the recognized tag set</customNote>
  public method Ping()
  end Ping;
end Widget;
