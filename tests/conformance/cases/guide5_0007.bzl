using var expired := new Array<Transaction>();
for var t in ledger do
  if t.IsExpired then expired.Add(t); end;   // collect borrows; mutate nothing yet
end;
for var t in expired do
  ledger.Remove(t);                          // structural changes outside the first loop
end;
