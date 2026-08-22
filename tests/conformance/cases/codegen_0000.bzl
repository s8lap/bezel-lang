module App.Billing;                       // first non-comment line; exactly one per file
import System.Collections.Generic;
import App.Ledger as Led;                 // optional file-scoped alias
type Handler<T> := (T, string?) -> bool;  // type alias: `:=`, never a visibility keyword
