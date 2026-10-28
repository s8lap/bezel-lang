module Conf;
// Currency: 0131 §3 row says "§15.4's fresh-store WARNING — in both builds". Since v0.346 (proposal 0207) it is an ERROR. EXPECT (spec v0.349): error. r35: measure.
public class Connection
  public constructor()
  end constructor;
end Connection;
public class Pool
  private owned _conns: Array<Connection>;
  public constructor()
    _conns := new Array<Connection>();
    _conns.Add(new Connection());
  end constructor;
end Pool;
