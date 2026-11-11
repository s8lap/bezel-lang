"""Python: one exception hierarchy, arranged by subject (OSError, LookupError, ValueError).
No type says "expected". A caller that expects a condition writes `except` for it at the
call (EAFP); a caller that does not writes nothing, and the traceback is the bug report."""
import socket
import sys


def call():
    with socket.create_connection(("127.0.0.1", 1), timeout=0.3):
        pass


# 1a. A file the user chose.
def open_user_file(path):
    try:
        with open(path, "rb") as f:
            print("user-file:", len(f.read()), "bytes")
    except FileNotFoundError:
        print("user-file: no such file, pick another")
    except OSError as e:
        print("user-file: cannot read:", e)


# 1b. The database the service cannot run without: the same FileNotFoundError.
def open_required_file(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        sys.exit(f"required-file: cannot start: {e}")  # exit status 1, no traceback


# 2a. Optional telemetry.   2b. The primary upstream: no handler here; the boundary has one.
def send_telemetry():
    try:
        call()
    except OSError:
        print("telemetry: dropped one sample")


def handle_request():
    call()


# 3a. A number the user typed.   3b. A constant the program wrote.
def parse_user(s):
    try:
        print("user-number:", int(s))
    except ValueError:
        print("user-number: not a number, ask again")


def parse_constant(s):
    return int(s)  # a ValueError here is a bug: no handler


# 4a. A user's key: catching KeyError is the idiom.   4b. The program's own key.
LIMITS = {"retries": 3}


def lookup_user(k):
    try:
        print("user-key:", LIMITS[k])
    except KeyError:
        print("user-key: unknown setting")


def lookup_own(k):
    return LIMITS[k]


mode = sys.argv[1]
if mode == "user-file":
    open_user_file("/nonexistent/notes.txt")
elif mode == "required-file":
    open_required_file("/nonexistent/service.db")
elif mode == "telemetry":
    send_telemetry()
elif mode == "primary-api":
    try:
        handle_request()
    except OSError:  # the request boundary
        print("primary-api: 502 Bad Gateway")
elif mode == "user-number":
    parse_user("12a")
elif mode == "constant-number":
    print("constant-number:", parse_constant("8080"))
elif mode == "constant-number-wrong":
    print(parse_constant("80a0"))
elif mode == "user-key":
    lookup_user("colour")
elif mode == "own-key":
    print("own-key:", lookup_own("retries"))
elif mode == "own-key-wrong":
    print(lookup_own("retires"))
