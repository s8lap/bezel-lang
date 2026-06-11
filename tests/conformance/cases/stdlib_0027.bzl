using var cacheEntry := new Shared<ParsedConfig>(Parse(text));
_index.Add("primary", cacheEntry.Clone());   // the index owns its own handle
                                             //   (_index: OwnedDictionary<string, Shared<ParsedConfig>>)
Apply(cacheEntry.Get());                     // borrow for the call
