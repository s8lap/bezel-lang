var processItem := (item: Item) => do
  var normalized := Normalize(item);
  var enriched := Enrich(normalized);
  return enriched;
end;
