public pure method Headroom(): decimal
  ensures(result >= 0m)
  return _balance + _limit;     // the §1 invariant guarantees non-negativity
end Headroom;
