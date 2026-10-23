# FFCSync — IPC Protocol

## Transport

- **Unix domain stream socket** (`AF_UNIX`, `SOCK_STREAM`). The default path is `/tmp/ffcsync.sock` (`--socket PATH`).
- The server creates the socket file on start.
  - If the file already exists and a server is listening on it, start-up fails ("another server is already listening").
  - If the file exists but nobody is listening (a stale file from a crash), it is removed and replaced.
- On graceful shutdown (SIGINT/SIGTERM) the server closes and **unlinks** the socket file.

## Framing

A stream socket has no message boundaries: a single `write` can arrive in several `read`s, and two writes can arrive in one. Every message is therefore framed:

```
+--------+--------+--------+--------+----------------------------+
|  payload length, uint32 big-endian |  payload (ASCII, ≤ 1024 B) |
+--------+--------+--------+--------+----------------------------+
```

- `read_frame` loops until it has all 4 header bytes, then all *length* bytes, so partial reads never corrupt a message.
  - EOF before the first header byte means the peer closed cleanly.
  - EOF in the middle of a frame is an error.
- `write_frame` builds header and payload into one buffer and loops until everything is sent. Each frame is written under the connection's write mutex, so frames from different worker threads never interleave.
- A length above 1024 is **not** recoverable, because the stream can no longer be resynchronized. The server replies `MALFORMED` (request id `?`) and closes that connection. Other clients are unaffected.
- `send(..., MSG_NOSIGNAL)` is used so that writing to a vanished client yields `EPIPE` instead of killing the server with `SIGPIPE`.

## Messages

Fields are separated by `|`. Identifiers (`request_id`, `student_id`, `section_id`) must be 1–64 characters from `[A-Za-z0-9-_.:]`, because they end up in CSV files.

### Request (client → server)

```
REQ|<request_id>|<ADD|DROP>|<student_id>|<section_id>
REQ|R07|ADD|S002|CSE2005-B1
```

`request_id` is chosen by the client and echoed back. It must be unique within one connection if the client pipelines requests.

### Response (server → client)

```
RES|<request_id>|<seq>|<ACCEPTED|REJECTED>|<reason>|<server_us>
RES|R07|7|ACCEPTED|OK|41
RES|R08|8|REJECTED|SECTION_FULL|38
RES|bad-1|0|REJECTED|MALFORMED|0
```

| Field | Meaning |
|---|---|
| `seq` | The FCFS sequence number assigned at admission. `0` means the message was never admitted (malformed). |
| `reason` | `OK` or one of the codes in `SCOPE.md` §2 |
| `server_us` | Microseconds from the moment a worker dequeued the request until it committed, including any wait for its commit turn. With `--workers 0`, this is the inline processing time. |

## Ordering and delivery

- **Exactly one response** is sent for every complete frame received.
- Requests sent on one connection are **admitted** in the order they were sent.
- With more than one worker, responses can **arrive** out of order, so clients must match them by `request_id`. `ffcsync_client` does this.
- If a client disconnects with requests already admitted, those requests still commit, and their responses are discarded.
- Malformed requests are answered immediately by the reader thread, and the connection stays open (except for the oversize-frame case above).

## Example session

```text
client → [00 00 00 1E] "REQ|demo-1|ADD|S005|HUM1021-A2"
server → [00 00 00 1B] "RES|demo-1|1|ACCEPTED|OK|12"
client → [00 00 00 1F] "REQ|demo-3|JOIN|S001|CSE2005-A1"
server → [00 00 00 21] "RES|demo-3|0|REJECTED|MALFORMED|0"
```
