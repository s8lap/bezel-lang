module Conf;
// s28_14_0032: scoped construction over a field's current value. EXPECT: quiet.
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
