// Rust: an operation that the outside world can defeat returns `Result`; the caller turns
// it into a value (`match`, `.ok()`), passes it up (`?`) or asserts it (`.expect`, `.unwrap`),
// which panics. Operations only a caller bug can defeat (`v[i]`, `map[k]`) panic, and each
// has a non-panicking twin (`get`). The library sets a default; the call site has the last word.
use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr, TcpStream};
use std::time::Duration;
use std::{env, fs, io};

fn call() -> io::Result<()> {
    let addr: SocketAddr = "127.0.0.1:1".parse().expect("hard-coded address is valid");
    TcpStream::connect_timeout(&addr, Duration::from_millis(300)).map(|_| ())
}

// 1a. A file the user chose.
fn open_user_file(path: &str) {
    match fs::read(path) {
        Ok(d) => println!("user-file: {} bytes", d.len()),
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            println!("user-file: no such file, pick another")
        }
        Err(e) => println!("user-file: cannot read: {e}"),
    }
}

// 1b. The database the service cannot run without: the same `io::Error`, asserted.
fn open_required_file(path: &str) -> Vec<u8> {
    fs::read(path).expect("required-file: the database must exist") // panic, exit status 101
}

// 2a. Optional telemetry: discard the error.   2b. The primary upstream: propagate with `?`.
fn send_telemetry() {
    if call().is_err() {
        println!("telemetry: dropped one sample");
    }
}

fn handle_request() -> io::Result<()> {
    call()?;
    Ok(())
}

// 3a. A number the user typed.   3b. A constant the program wrote.
fn parse_user(s: &str) {
    match s.parse::<i32>() {
        Ok(n) => println!("user-number: {n}"),
        Err(_) => println!("user-number: not a number, ask again"),
    }
}

fn parse_constant(s: &str) -> i32 {
    s.parse().expect("constant-number: the literal is a valid integer")
}

// 4a. A key the user supplied: `get` returns `Option`.   4b. The program's own key: index.
fn lookup_user(m: &HashMap<&str, i32>, k: &str) {
    match m.get(k) {
        Some(v) => println!("user-key: {v}"),
        None => println!("user-key: unknown setting"),
    }
}

fn lookup_own(m: &HashMap<&str, i32>, k: &str) -> i32 {
    m[k] // panics when absent
}

fn main() {
    let limits = HashMap::from([("retries", 3)]);
    let _loopback: IpAddr = "127.0.0.1".parse().expect("hard-coded address is valid");
    match env::args().nth(1).as_deref() {
        Some("user-file") => open_user_file("/nonexistent/notes.txt"),
        Some("required-file") => {
            open_required_file("/nonexistent/service.db");
        }
        Some("telemetry") => send_telemetry(),
        Some("primary-api") => {
            if handle_request().is_err() {
                println!("primary-api: 502 Bad Gateway") // the request boundary
            }
        }
        Some("user-number") => parse_user("12a"),
        Some("constant-number") => println!("constant-number: {}", parse_constant("8080")),
        Some("constant-number-wrong") => println!("{}", parse_constant("80a0")),
        Some("user-key") => lookup_user(&limits, "colour"),
        Some("own-key") => println!("own-key: {}", lookup_own(&limits, "retries")),
        Some("own-key-wrong") => println!("{}", lookup_own(&limits, "retires")),
        _ => {}
    }
}
