using var a := new Buffer();          // new T(...)            -> owned
var b := source.OpenBuffer();         // returns owned Buffer   -> owned
var c := source.Buffer;               // field / property read  -> borrowed (source: source)
var d := registry.Find("key");        // returns Buffer (no owned) -> borrowed
var t := source.PrefetchAsync();      // returns Task<Buffer>  -> owned (task calls mint, v0.190)
var q := xs.Where(p);                 // returns a lazy query  -> owned (calls mint, v0.258)
var e := move holder.slot;            // move out of owned field -> owned
