var cfg := LoadAsync().Result;    // ERROR inside async code: blocks its own domain — await it
