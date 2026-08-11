import System.Diagnostics;

public method Rebalance()
  // … move entries between _hot and _cold …
  Debug.Assert(_hot.Count + _cold.Count = _total, "rebalance lost entries");
end Rebalance;
