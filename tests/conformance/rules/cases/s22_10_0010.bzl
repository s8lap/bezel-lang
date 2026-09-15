//! case: s22_10_0010
//! doc: spec
//! section: 22.10
//! title: an owned primary parameter used in an initializer and a body position is rejected
//! rule: Referenced both in an owned-consuming initializer and in a body
//!   position: compile error — the body reference is a read of a
//!   moved-from value.
//! cites: 22.10
//! expect: reject
//! diagnostic: moved-from
module Conf;

public class Bad(owned resource: Stream)
  private owned _resource: Stream := resource;

  public method Touch()
    resource.Flush();
  end Touch;
end Bad;
