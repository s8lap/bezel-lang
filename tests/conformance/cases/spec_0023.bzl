record Interval
  public readonly property Lo: int;
  public readonly property Hi: int;
  private equality skip _cachedLen: int;    // excluded from =, Equals, GetHashCode —
                                            //   and the package survives the field
  public constructor(lo: int, hi: int)
    Lo := lo;
    Hi := hi;
    _cachedLen := hi - lo;
  end constructor;
end Interval;
// new Interval(1, 5) = new Interval(1, 5) is true whatever the cache states.
