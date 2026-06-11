var toRemove := new Array<Transaction>();
for var t in transactions do
  if t.IsExpired then toRemove.Add(t); end;
end;
for var t in toRemove do
  transactions.Remove(t);
end;
