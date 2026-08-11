var labels := entries.Select(lambda(Entry(k, v)) => $"{k}: {v}");
var positives := points.Where(lambda(Point(x, _)) => x > 0);
