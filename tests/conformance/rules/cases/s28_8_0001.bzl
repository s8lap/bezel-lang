//! case: s28_8_0001
//! doc: spec
//! section: 28.8
//! title: an owned parameter transfers ownership and may be stored into an owned field directly
//! rule: When a parameter is `owned`, the caller relinquishes ownership; the
//!   owned-field assignment drop-on-overwrites the displaced value, so a
//!   replacement method needs no explicit dispose-first step.
//! cites: 28.8, 28.7
//! expect: accept
module Conf;

public class Resource
  public constructor()
  end constructor;
end Resource;

public class Holder
  private owned _resource: Resource;

  public constructor()
    _resource := new Resource();
  end constructor;

  public method ReplaceResource(owned resource: Resource)
    _resource := resource;
  end ReplaceResource;
end Holder;
