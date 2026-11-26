//! case: s28_14_3605
//! doc: spec
//! section: 28.14
//! title: an Add forward of a scoped parameter inside a locally-invoked lambda is rejected like its += twin
//! rule: it reads the statement after §17.2's fixed desugaring: `lambda() => do _changed += handler; end`
//!   and `lambda() => do _changed.Add(handler); end` are the same forward and are both rejected
//! cites: 28.14, 17.2
//! expect: reject
//! diagnostic: scoped
//! issue: 9311
module Conf;

public class Hub
  private owned readonly _changed: EventSource<double> := new EventSource<double>();

  public method Fire(t: double)
    _changed.Raise(t);
  end Fire;

  public method On(scoped handler: (double) -> ())
    var f := lambda() => do _changed.Add(handler); end;
    f();
  end On;
end Hub;
