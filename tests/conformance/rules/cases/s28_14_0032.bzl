//! case: s28_14_0032
//! doc: spec
//! section: 28.14
//! title: under the report mode, a scoped construction over a field's current value reports nothing
//! rule: not a new admission and not a K4.3 clause — the trust-admission report
//!   (§28.3) stays silent at a scoped construction
//! cites: 28.14, 28.3
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 4605
module Conf;

public class Source
  public constructor()
  end constructor;

  public method Read(): int => 7;
end Source;

public class Reader
  private _src: Source;

  public constructor(scoped src: Source)
    _src := src;
  end constructor;

  public method Next(): int => _src.Read();
end Reader;

public class Host
  private owned _s: Source;

  public constructor()
    _s := new Source();
  end constructor;

  public method Pump(): int
    using var r := new Reader(scoped _s);
    return r.Next();
  end Pump;
end Host;
