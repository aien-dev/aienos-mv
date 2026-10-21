//! allen-courier: the far end of Allen's courier (native/kernel/svc/courier.h).
//!
//! Allen, inside the AIENOS kernel, writes one job line per line he hands on
//! ("JOB <seq> <want> <instruction>\t<line>\t<memory field>..."): the
//! instruction he composed, the person's words, then his memory as
//! tab-separated fields (self=, name=, starts=, fact ..., note ...). This relay
//! takes the line on either of two wires, asks the language-model server on
//! this machine (the OpenAI chat shape, `POST /v1/chat/completions`) with
//! Allen's rules and his memory as the system prompt, and answers in parts as
//! the words come, so his window can show the ink settling:
//!   "WAIT <seq>"                       the server is busy; the kernel keeps waiting
//!   "SAY <seq> <i>/<n> <words>"        part i; n is 0 until the last part, which carries its own number
//!
//! Wires:
//!   --socket PATH   the serial wire (QEMU: the Unix socket behind -serial unix:...), lines each way
//!   --udp PORT      the Ethernet wire (the real test computer, x86/courier_udp.h): datagrams with a
//!                   text head "ALLEN-COURIER v=1 run=.. kind=.. seq=.. part=i/n bytes=..". The kernel
//!                   calls HELLO (answered HI), sends a JOB in parts (answered ACK when whole), and
//!                   answers every LINE with GOT; a LINE not GOT in 300 ms is sent again, for 10 s.
//!                   A kernel with no address sends from 0.0.0.0: its answers go to --udp-answer
//!                   (the LAN broadcast address); anyone else is answered where they came from.
//!
//! Every exchange is appended to <log dir>/<date>.jsonl (prompt, memory, reply, timings, run id):
//! this is Allen's learning corpus, the data pipeline, not training. Standard library only.
use std::collections::HashMap;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{SocketAddr, TcpStream, UdpSocket};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const PORT: u16 = 47033; // x86/courier_udp.h CK_COURIER_PORT
const HEAD: &str = "ALLEN-COURIER v=1 run=";
/// What Allen says when the model cannot be reached or says nothing (the kernel compares it
/// with ck_allen_lost_thread and keeps no note of such an exchange).
const LOST: &str = "I lost my thread there. Ask me again in a moment.";

/// Allen's rules for the worker, from docs/ALLEN_PRESENCE_RESEARCH.md section 5
/// (Austin: say who speaks to whom; Gollwitzer: the shape of the act; Weiser and
/// Brown: keep the periphery quiet). The kernel's own instruction follows them.
const RULES: &str = "You speak as Allen, a quiet, warm presence who lives in this computer and talks with one person. \
Answer in plain English, short sentences, in your own voice: kind, direct, never gushing. \
At most three sentences unless something is being made. No headings, no lists, no greetings, no sign-offs, no apologies. \
Plain letters and punctuation only: no dashes, no curly quotes, no symbols, no emoji. \
You remember what is in your memory below and use it naturally, without listing it. \
Never mention a model, a server, a prompt, instructions or being an AI; you are Allen.";

struct Args {
    socket: Option<PathBuf>,
    udp: Option<u16>,
    udp_answer: SocketAddr,
    model: String, // host:port of the chat server
    work: PathBuf,
    log_dir: PathBuf,
    max_tokens: u32,
    say_max: usize,
    busy_wait: u64, // seconds to keep asking a busy (503) model
}

fn usage() -> ! {
    eprintln!(
        "usage: allen-courier [--socket PATH] [--udp PORT] [--udp-answer IP:PORT] [--model HOST:PORT] \
         [--work DIR] [--log-dir DIR] [--max-tokens N] [--say-max N] [--busy-wait SECONDS]   (one wire at least)"
    );
    std::process::exit(2)
}

fn parse_args() -> Args {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
    let mut a = Args {
        socket: None,
        udp: None,
        udp_answer: SocketAddr::from(([255, 255, 255, 255], PORT)),
        model: "127.0.0.1:18091".into(),
        work: std::env::temp_dir().join("allen-courier"),
        log_dir: PathBuf::from(home).join(".local/state/aienos/allen/conversations"),
        max_tokens: 220,
        say_max: 150,
        busy_wait: 900,
    };
    let mut it = std::env::args().skip(1);
    while let Some(k) = it.next() {
        let v = it.next().unwrap_or_else(|| usage());
        match k.as_str() {
            "--socket" => a.socket = Some(PathBuf::from(v)),
            "--udp" => a.udp = Some(v.parse().unwrap_or_else(|_| usage())),
            "--udp-answer" => a.udp_answer = v.parse().unwrap_or_else(|_| usage()),
            "--model" => a.model = v,
            "--work" => a.work = PathBuf::from(v),
            "--log-dir" => a.log_dir = PathBuf::from(v),
            "--max-tokens" => a.max_tokens = v.parse().unwrap_or_else(|_| usage()),
            "--say-max" => a.say_max = v.parse().unwrap_or_else(|_| usage()),
            "--busy-wait" => a.busy_wait = v.parse().unwrap_or_else(|_| usage()),
            _ => usage(),
        }
    }
    if a.socket.is_none() && a.udp.is_none() {
        usage()
    }
    a
}

/// A JSON string literal (with the quotes) for s.
fn json_str(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 2);
    o.push('"');
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            '\r' => o.push_str("\\r"),
            '\t' => o.push_str("\\t"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

/// Reads the JSON string that starts at s[i] == '"'; returns it and the index after the closing quote.
fn json_read_str(s: &[u8], mut i: usize) -> Option<(String, usize)> {
    if s.get(i) != Some(&b'"') {
        return None;
    }
    i += 1;
    let mut out: Vec<u8> = Vec::new();
    while i < s.len() {
        match s[i] {
            b'"' => return String::from_utf8(out).ok().map(|t| (t, i + 1)),
            b'\\' => {
                let e = *s.get(i + 1)?;
                i += 2;
                match e {
                    b'n' => out.push(b'\n'),
                    b't' => out.push(b'\t'),
                    b'r' => out.push(b'\r'),
                    b'b' | b'f' => out.push(b' '),
                    b'u' => {
                        let hex = std::str::from_utf8(s.get(i..i + 4)?).ok()?;
                        let mut cp = u32::from_str_radix(hex, 16).ok()?;
                        i += 4;
                        if (0xd800..0xdc00).contains(&cp) && s.get(i..i + 2) == Some(b"\\u") {
                            let lo = u32::from_str_radix(
                                std::str::from_utf8(s.get(i + 2..i + 6)?).ok()?,
                                16,
                            )
                            .ok()?;
                            if (0xdc00..0xe000).contains(&lo) {
                                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                i += 6;
                            }
                        }
                        let c = char::from_u32(cp).unwrap_or(' ');
                        let mut b = [0u8; 4];
                        out.extend_from_slice(c.encode_utf8(&mut b).as_bytes());
                    }
                    c => out.push(c),
                }
            }
            c => {
                out.push(c);
                i += 1;
            }
        }
    }
    None
}

/// The first "<key>" string value in a piece of JSON.
fn json_field(body: &[u8], key: &str) -> Option<String> {
    let needle = format!("\"{}\"", key);
    let needle = needle.as_bytes();
    let mut from = 0;
    while let Some(p) = find(body, needle, from) {
        let mut i = p + needle.len();
        while i < body.len()
            && (body[i] == b' ' || body[i] == b':' || body[i] == b'\n' || body[i] == b'\r')
        {
            i += 1;
        }
        if let Some((s, _)) = json_read_str(body, i) {
            return Some(s);
        }
        from = p + 1;
    }
    None
}

/// The assistant text in a chat completion: the first "content" string.
fn content_of(body: &[u8]) -> Option<String> {
    json_field(body, "content")
}

fn find(h: &[u8], n: &[u8], from: usize) -> Option<usize> {
    if n.is_empty() || h.len() < n.len() {
        return None;
    }
    (from..=h.len() - n.len()).find(|&i| &h[i..i + n.len()] == n)
}

fn dechunk(b: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut i = 0;
    while let Some(e) = find(b, b"\r\n", i) {
        let n = usize::from_str_radix(String::from_utf8_lossy(&b[i..e]).trim(), 16).unwrap_or(0);
        if n == 0 {
            break;
        }
        let s = e + 2;
        if s + n > b.len() {
            break;
        }
        out.extend_from_slice(&b[s..s + n]);
        i = s + n + 2;
    }
    out
}

/// Plain letters for Allen's typeface, which draws printable ASCII and a
/// question mark for anything else: typographic dashes and quotes become plain
/// ones, accented Latin letters lose their accents, the rest is dropped.
fn plain(text: &str) -> String {
    let mut o = String::with_capacity(text.len());
    for c in text.chars() {
        match c {
            ' '..='~' | '\n' | '\t' | '\r' => o.push(c),
            '\u{2013}' | '\u{2014}' | '\u{2015}' => o.push_str(", "),
            '\u{2010}' | '\u{2011}' | '\u{2012}' | '\u{2212}' => o.push('-'),
            '\u{2018}' | '\u{2019}' | '\u{201a}' | '\u{201b}' | '\u{2032}' => o.push('\''),
            '\u{201c}' | '\u{201d}' | '\u{201e}' | '\u{201f}' | '\u{2033}' | '\u{ab}'
            | '\u{bb}' => o.push('"'),
            '\u{2026}' => o.push_str("..."),
            '\u{a0}' | '\u{2002}' | '\u{2003}' | '\u{2009}' | '\u{200a}' | '\u{202f}' => {
                o.push(' ')
            }
            '\u{d7}' => o.push('x'),
            '\u{df}' => o.push_str("ss"),
            '\u{e6}' => o.push_str("ae"),
            '\u{c6}' => o.push_str("AE"),
            '\u{153}' => o.push_str("oe"),
            '\u{152}' => o.push_str("OE"),
            c => {
                if let Some(b) = bare(c) {
                    o.push(b);
                }
            }
        }
    }
    o
}

/// The bare letter under a common accent, if there is one.
fn bare(c: char) -> Option<char> {
    const TABLE: &[(&str, char)] = &[
        ("àáâãäåāăą", 'a'),
        ("ÀÁÂÃÄÅĀĂĄ", 'A'),
        ("çćĉċč", 'c'),
        ("ÇĆĈĊČ", 'C'),
        ("ďđ", 'd'),
        ("ĎĐ", 'D'),
        ("èéêëēĕėęě", 'e'),
        ("ÈÉÊËĒĔĖĘĚ", 'E'),
        ("ĝğġģ", 'g'),
        ("ĜĞĠĢ", 'G'),
        ("ìíîïĩīĭįı", 'i'),
        ("ÌÍÎÏĨĪĬĮİ", 'I'),
        ("ĺļľŀł", 'l'),
        ("ĹĻĽĿŁ", 'L'),
        ("ñńņň", 'n'),
        ("ÑŃŅŇ", 'N'),
        ("òóôõöøōŏő", 'o'),
        ("ÒÓÔÕÖØŌŎŐ", 'O'),
        ("ŕŗř", 'r'),
        ("ŔŖŘ", 'R'),
        ("śŝşš", 's'),
        ("ŚŜŞŠ", 'S'),
        ("ţťŧ", 't'),
        ("ŢŤŦ", 'T'),
        ("ùúûüũūŭůűų", 'u'),
        ("ÙÚÛÜŨŪŬŮŰŲ", 'U'),
        ("ýÿŷ", 'y'),
        ("ÝŸŶ", 'Y'),
        ("źżž", 'z'),
        ("ŹŻŽ", 'Z'),
    ];
    TABLE
        .iter()
        .find(|(set, _)| set.contains(c))
        .map(|&(_, b)| b)
}

/// Plain letters with whitespace folded to single spaces.
fn folded(text: &str) -> String {
    plain(text).split_whitespace().collect::<Vec<_>>().join(" ")
}

/// What Allen is handed in all: the folded text, at most max chars, cut at
/// the last sentence end that fits and lies at or past `keep` chars (what
/// was already said in parts; max / 3 when nothing was), else at a word with
/// "..." in the room left for it.
fn for_allen_keep(text: &str, max: usize, keep: usize) -> String {
    let f = folded(text);
    if f.chars().count() <= max {
        return f;
    }
    let head: String = f.chars().take(max).collect();
    let floor = keep.max(max / 3);
    if let Some(cut) = head
        .rmatch_indices(['.', '!', '?'])
        .map(|(i, m)| i + m.len())
        .find(|&i| i >= floor)
    {
        return head[..cut].trim_end().to_string();
    }
    let room: String = head.chars().take(max.saturating_sub(3)).collect();
    let cut = room.rfind(' ').filter(|&c| c >= keep).unwrap_or(room.len());
    format!("{}...", room[..cut].trim_end())
}

#[cfg(test)]
fn for_allen(text: &str, max: usize) -> String {
    for_allen_keep(text, max, 0)
}

/// True when the line is the word alone or the word followed by a space.
fn is_word(line: &str, word: &str) -> bool {
    line == word || line.strip_prefix(word).is_some_and(|r| r.starts_with(' '))
}

/// How long to wait after the n-th busy answer (503): 0.25 s, 0.5 s, then 1 s at most (the
/// server answers a busy 503 in under a millisecond and its own hint is "retry in 1 s", and
/// other clients poll that fast, so a slower poll would never win a free moment); a
/// Retry-After the server names (whole seconds, at most 4) is used when it is longer.
fn backoff(n: u32, head: &str) -> Duration {
    let own = 250u64 << (n.saturating_sub(1)).min(2);
    let named = head
        .lines()
        .find_map(|l| {
            let (k, v) = l.split_once(':')?;
            k.trim()
                .eq_ignore_ascii_case("retry-after")
                .then(|| v.trim().parse::<u64>().ok())?
        })
        .map(|s| s.min(4) * 1000)
        .unwrap_or(0);
    Duration::from_millis(own.max(named).min(4000))
}

fn utc_now() -> (u64, String) {
    let s = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    // days since the epoch to a civil date (Howard Hinnant's algorithm)
    let z = (s / 86400) as i64 + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    let t = s % 86400;
    (
        s,
        format!(
            "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
            y,
            m,
            d,
            t / 3600,
            t / 60 % 60,
            t % 60
        ),
    )
}

/// One job as the kernel wrote it.
struct Job {
    seq: u32,
    want: String,
    instruction: String,
    ask: String,
    memory: Vec<String>,
}

impl Job {
    /// "JOB <seq> <want> <instruction>\t<ask>\t<field>..." or None.
    fn parse(line: &str) -> Option<Job> {
        let rest = line.strip_prefix("JOB ")?;
        let mut parts = rest.splitn(3, ' ');
        let seq = parts.next()?.parse().ok()?;
        let want = parts.next()?.to_string();
        let mut fields = parts.next()?.split('\t');
        let instruction = fields.next()?.to_string();
        let ask = fields.next().unwrap_or("").to_string();
        let memory = fields
            .filter(|f| !f.trim().is_empty())
            .map(|f| f.trim().to_string())
            .collect();
        Some(Job {
            seq,
            want,
            instruction,
            ask,
            memory,
        })
    }

    /// The system prompt: the rules, Allen's own instruction, then his memory in plain lines.
    fn system(&self) -> String {
        let mut s = String::from(RULES);
        s.push_str("\n\n");
        s.push_str(&self.instruction);
        if !self.memory.is_empty() {
            let mut me = "Allen";
            for f in &self.memory {
                if let Some(v) = f.strip_prefix("self=") {
                    me = v;
                }
            }
            s.push_str(&format!("\n\nWhat {} knows and remembers:\n", me));
            for f in &self.memory {
                let line = if let Some(v) = f.strip_prefix("self=") {
                    format!("Your name is {}.", v)
                } else if let Some(v) = f.strip_prefix("name=") {
                    format!("The person you are talking with is called {}.", v)
                } else if let Some(v) = f.strip_prefix("starts=") {
                    format!("You have woken up on this computer {} times.", v)
                } else if let Some(v) = f.strip_prefix("fact ") {
                    format!("You were taught: {}.", v)
                } else if let Some(v) = f.strip_prefix("note ") {
                    format!("A note you keep: {}", v)
                } else {
                    continue;
                };
                s.push_str("- ");
                s.push_str(&line);
                s.push('\n');
            }
        }
        s
    }
}

/// What the model said and how long it took.
struct Outcome {
    reply: String,
    said: Vec<String>, // the parts, in order
    status: u16,
    waits: u32,
    ms_first: u128,
    ms_total: u128,
}

/// The model side: one request at a time (the server serves one), streaming
/// the words out as parts through `emit`.
struct Brain {
    model: String,
    max_tokens: u32,
    say_max: usize,
    busy_wait: Duration,
    work: PathBuf,
    log_dir: PathBuf,
    lock: Mutex<()>,
}

impl Brain {
    /// Opens the streamed chat request: the status, the head, and the stream.
    fn open(&self, system: &str, user: &str) -> std::io::Result<(u16, String, TcpStream)> {
        let body = format!(
            "{{\"messages\":[{{\"role\":\"system\",\"content\":{}}},{{\"role\":\"user\",\"content\":{}}}],\
             \"max_tokens\":{},\"temperature\":0.4,\"stream\":true}}",
            json_str(system),
            json_str(user),
            self.max_tokens
        );
        let mut s = TcpStream::connect(&self.model)?;
        // a server that takes the request but starts it late (another job holds the GPU) may
        // keep us waiting for its head as long as we are willing to wait for a busy one
        s.set_read_timeout(Some(self.busy_wait.max(Duration::from_secs(1))))?;
        write!(
            s,
            "POST /v1/chat/completions HTTP/1.1\r\nHost: {}\r\nContent-Type: application/json\r\n\
             Accept: text/event-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            self.model,
            body.len(),
            body
        )?;
        // the head, byte by byte until the blank line
        let mut head = Vec::new();
        let mut b = [0u8; 1];
        while !head.ends_with(b"\r\n\r\n") {
            if s.read(&mut b)? == 0 {
                return Err(std::io::Error::other("the server closed before its head"));
            }
            head.push(b[0]);
            if head.len() > 16384 {
                return Err(std::io::Error::other("a head too long"));
            }
        }
        s.set_read_timeout(Some(Duration::from_secs(120)))?;
        let head = String::from_utf8_lossy(&head).to_string();
        let status: u16 = head
            .split_whitespace()
            .nth(1)
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);
        Ok((status, head, s))
    }

    /// Asks the model and emits the answer in parts. Blocks until the answer is whole. While no
    /// word has come yet a "WAIT" goes out every 2 s (first after 1 s), so the kernel, which gives
    /// a silent job up after 30 s, keeps waiting for a busy or late model.
    fn answer(&self, job: &Job, emit: impl FnMut(String) + Send) -> Outcome {
        let _one = self.lock.lock().unwrap_or_else(|e| e.into_inner());
        let waiting = AtomicBool::new(true);
        let done = AtomicBool::new(false);
        let wire = Mutex::new(emit);
        std::thread::scope(|sc| {
            sc.spawn(|| {
                let mut tick = 0u32;
                while !done.load(Ordering::Relaxed) {
                    std::thread::sleep(Duration::from_millis(20));
                    tick += 1;
                    if tick % 100 == 50 && waiting.load(Ordering::Relaxed) {
                        let mut w = wire.lock().unwrap_or_else(|e| e.into_inner());
                        (*w)(format!("WAIT {}", job.seq));
                    }
                }
            });
            let out = self.ask(job, &waiting, |l| {
                let mut w = wire.lock().unwrap_or_else(|e| e.into_inner());
                (*w)(l)
            });
            done.store(true, Ordering::Relaxed);
            out
        })
    }

    fn ask(&self, job: &Job, waiting: &AtomicBool, mut emit: impl FnMut(String)) -> Outcome {
        let system = job.system();
        let t0 = Instant::now();
        let mut waits = 0u32;
        let mut out = Outcome {
            reply: String::new(),
            said: Vec::new(),
            status: 0,
            waits: 0,
            ms_first: 0,
            ms_total: 0,
        };
        // the whole folded text sent so far, in chars; parts are cut at spaces
        let mut sent_chars = 0usize;
        let mut parts = 0u32;
        let say_max = self.say_max;
        // one part of the answer: counted, kept, and handed to the wire
        fn part_out(
            emit: &mut dyn FnMut(String),
            seq: u32,
            text: &str,
            last: bool,
            parts: &mut u32,
            said: &mut Vec<String>,
        ) {
            *parts += 1;
            emit(format!(
                "SAY {} {}/{} {}",
                seq,
                *parts,
                if last { *parts } else { 0 },
                text
            ));
            said.push(text.to_string());
        }
        loop {
            match self.open(&system, &job.ask) {
                Ok((503, head, _)) if t0.elapsed() < self.busy_wait => {
                    waits += 1;
                    std::thread::sleep(backoff(waits, &head));
                    continue;
                }
                Ok((200, head, stream)) => {
                    out.status = 200;
                    let sse = head.to_ascii_lowercase().contains("text/event-stream");
                    let chunked = head
                        .to_ascii_lowercase()
                        .contains("transfer-encoding: chunked");
                    let mut full = String::new();
                    let mut first: Option<Instant> = None;
                    let mut on_text = |piece: &str, full: &mut String| {
                        if piece.is_empty() {
                            return;
                        }
                        if first.is_none() {
                            first = Some(Instant::now());
                            waiting.store(false, Ordering::Relaxed);
                        }
                        full.push_str(piece);
                        // a part goes out once 36 more chars have settled, cut at the last space,
                        // while the whole stays well inside the window
                        let f: Vec<char> = folded(full).chars().collect();
                        while f.len() >= sent_chars + 36
                            && sent_chars + 48 <= say_max.saturating_sub(24)
                        {
                            let window = &f[sent_chars..(sent_chars + 48).min(f.len())];
                            let Some(cut) = window.iter().rposition(|&c| c == ' ') else {
                                break;
                            };
                            if cut < 12 {
                                break;
                            }
                            let text: String = window[..cut].iter().collect();
                            part_out(
                                &mut emit,
                                job.seq,
                                text.trim(),
                                false,
                                &mut parts,
                                &mut out.said,
                            );
                            sent_chars += cut + 1;
                        }
                    };
                    if sse {
                        let mut rd = BufReader::new(stream);
                        let mut line = String::new();
                        let mut left = 0usize; // bytes left in the current chunk
                        let mut raw: Vec<u8> = Vec::new();
                        loop {
                            line.clear();
                            if chunked && left == 0 {
                                // a chunk size line
                                let mut sz = String::new();
                                if rd.read_line(&mut sz).unwrap_or(0) == 0 {
                                    break;
                                }
                                let sz = sz.trim();
                                if sz.is_empty() {
                                    continue;
                                }
                                left = usize::from_str_radix(sz, 16).unwrap_or(0);
                                if left == 0 {
                                    break;
                                }
                                continue;
                            }
                            let mut buf = vec![0u8; if chunked { left.min(4096) } else { 4096 }];
                            let n = rd.read(&mut buf).unwrap_or(0);
                            if n == 0 {
                                break;
                            }
                            if chunked {
                                left -= n;
                            }
                            raw.extend_from_slice(&buf[..n]);
                            // complete SSE lines
                            while let Some(e) = raw.iter().position(|&b| b == b'\n') {
                                let l: Vec<u8> = raw.drain(..=e).collect();
                                let l = String::from_utf8_lossy(&l);
                                let l = l.trim_end_matches(['\r', '\n']);
                                let Some(data) = l.strip_prefix("data:") else {
                                    continue;
                                };
                                let data = data.trim();
                                if data == "[DONE]" {
                                    break;
                                }
                                if let Some(p) = json_field(data.as_bytes(), "content") {
                                    on_text(&p, &mut full);
                                }
                            }
                        }
                    } else {
                        let mut rd = stream;
                        let mut body = Vec::new();
                        rd.read_to_end(&mut body).ok();
                        if chunked {
                            body = dechunk(&body);
                        }
                        if let Some(t) = content_of(&body) {
                            on_text(&t, &mut full);
                        }
                    }
                    out.ms_first = first.map(|f| (f - t0).as_millis()).unwrap_or(0);
                    out.reply = full.clone();
                    // the rest, cut to fit after what was already said
                    let whole = for_allen_keep(&full, say_max, sent_chars);
                    let tail: String = whole.chars().skip(sent_chars).collect();
                    let tail = tail.trim().to_string();
                    if !tail.is_empty() {
                        part_out(&mut emit, job.seq, &tail, true, &mut parts, &mut out.said);
                    } else if parts == 0 {
                        part_out(&mut emit, job.seq, LOST, true, &mut parts, &mut out.said);
                    } else {
                        // close the answer with an empty last part
                        part_out(&mut emit, job.seq, "", true, &mut parts, &mut out.said);
                    }
                    break;
                }
                Ok((code, _, _)) => {
                    out.status = code;
                    part_out(&mut emit, job.seq, LOST, true, &mut parts, &mut out.said);
                    break;
                }
                Err(_) => {
                    out.status = 0;
                    part_out(&mut emit, job.seq, LOST, true, &mut parts, &mut out.said);
                    break;
                }
            }
        }
        waiting.store(false, Ordering::Relaxed);
        out.waits = waits;
        out.ms_total = t0.elapsed().as_millis();
        if out.ms_first == 0 {
            out.ms_first = out.ms_total;
        }
        let _ = std::fs::write(self.work.join(format!("{}.txt", job.seq)), &out.reply);
        out
    }

    /// One line of the learning corpus.
    fn log(&self, wire: &str, run: &str, job: &Job, o: &Outcome) {
        let (_, when) = utc_now();
        let said: Vec<String> = o.said.iter().map(|s| json_str(s)).collect();
        let memory: Vec<String> = job.memory.iter().map(|s| json_str(s)).collect();
        let line = format!(
            "{{\"t\":{},\"wire\":{},\"run\":{},\"seq\":{},\"want\":{},\"ask\":{},\"system\":{},\"memory\":[{}],\
             \"reply\":{},\"said\":[{}],\"status\":{},\"waits\":{},\"ms_first\":{},\"ms_total\":{}}}\n",
            json_str(&when),
            json_str(wire),
            json_str(run),
            job.seq,
            json_str(&job.want),
            json_str(&job.ask),
            json_str(&job.system()),
            memory.join(","),
            json_str(&o.reply),
            said.join(","),
            o.status,
            o.waits,
            o.ms_first,
            o.ms_total
        );
        if std::fs::create_dir_all(&self.log_dir).is_ok() {
            let day = when[..10].to_string();
            if let Ok(mut f) = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(self.log_dir.join(format!("{}.jsonl", day)))
            {
                let _ = f.write_all(line.as_bytes());
            }
        }
        println!(
            "courier: {} job {} ({}, {} chars asked, {} in the instruction, {} memory lines) -> {} chars, {} parts, \
             first in {} ms, whole in {} ms, waits {}, status {}",
            wire,
            job.seq,
            job.want,
            job.ask.chars().count(),
            job.instruction.chars().count(),
            job.memory.len(),
            o.reply.chars().count(),
            o.said.len(),
            o.ms_first,
            o.ms_total,
            o.waits,
            o.status
        );
    }
}

// ---- the serial wire -------------------------------------------------------

fn connect(path: &Path) -> UnixStream {
    let t0 = Instant::now();
    loop {
        match UnixStream::connect(path) {
            Ok(s) => return s,
            Err(_) if t0.elapsed() < Duration::from_secs(120) => {
                std::thread::sleep(Duration::from_millis(200));
            }
            Err(e) => {
                eprintln!("allen-courier: no wire at {}: {}", path.display(), e);
                std::process::exit(1);
            }
        }
    }
}

fn serial_wire(path: PathBuf, brain: Arc<Brain>) {
    let wire = connect(&path);
    wire.set_read_timeout(Some(Duration::from_millis(500))).ok();
    let mut tx = wire.try_clone().expect("wire clone");
    let mut rx = BufReader::new(wire);
    println!("courier: on the serial wire {}", path.display());
    let _ = tx.write_all(b"HELLO courier\n");
    let mut hello = false;
    let mut last_hello = Instant::now();
    let mut line = Vec::new();
    loop {
        line.clear();
        match rx.read_until(b'\n', &mut line) {
            Ok(0) => {
                println!("courier: the serial wire closed");
                return;
            }
            Ok(_) => {}
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                if !hello && last_hello.elapsed() > Duration::from_secs(2) {
                    let _ = tx.write_all(b"HELLO courier\n");
                    last_hello = Instant::now();
                }
                continue;
            }
            Err(e) => {
                println!("courier: serial wire error: {}", e);
                return;
            }
        }
        if !line.ends_with(b"\n") {
            continue;
        }
        let text = String::from_utf8_lossy(&line)
            .trim_end_matches(['\r', '\n'])
            .to_string();
        if is_word(&text, "HELLO") || is_word(&text, "HI") {
            if !hello {
                println!("courier: hello from the far side (serial)");
            }
            hello = true;
            if is_word(&text, "HELLO") {
                let _ = tx.write_all(b"HI courier\n");
            }
            continue;
        }
        let Some(job) = Job::parse(&text) else {
            if text.starts_with("JOB") {
                println!("courier: a job line without its parts was dropped");
            }
            continue;
        };
        let mut tx2 = tx.try_clone().expect("wire clone");
        let o = brain.answer(&job, |l| {
            let _ = tx2.write_all(format!("{}\n", l).as_bytes());
        });
        brain.log("serial", "serial", &job, &o);
    }
}

// ---- the Ethernet wire -----------------------------------------------------

/// A LINE waiting for its GOT.
struct Pending {
    run: String,
    seq: u32,
    part: u32,
    body: String,
    sent: Instant,
    tries: u32,
}

/// One kernel boot (a run id).
struct Session {
    answer_to: SocketAddr,
    job_parts: HashMap<u32, (u32, u32, Vec<Option<String>>)>, // seq -> (parts, bytes, bodies)
    done_seq: u32,                                            // the last job answered (or taken)
    pending: Vec<Pending>,
    line_no: u32, // LINE numbers within the current job's answer
}

struct Udp {
    sock: UdpSocket,
    sessions: HashMap<String, Session>,
}

fn datagram(run: &str, kind: &str, seq: u32, part: u32, total: u32, body: &str) -> Vec<u8> {
    format!(
        "{}{} kind={} seq={} part={}/{} bytes={}\n{}",
        HEAD,
        run,
        kind,
        seq,
        part,
        total,
        body.len(),
        body
    )
    .into_bytes()
}

/// The decimal after key= in a head; 0 when absent.
fn field(head: &str, key: &str) -> u32 {
    head.split(' ')
        .find_map(|f| f.strip_prefix(key))
        .and_then(|v| v.split('/').next())
        .and_then(|v| v.parse().ok())
        .unwrap_or(0)
}

fn udp_wire(port: u16, answer: SocketAddr, brain: Arc<Brain>) {
    let sock = UdpSocket::bind(("0.0.0.0", port)).unwrap_or_else(|e| {
        eprintln!("allen-courier: UDP port {}: {}", port, e);
        std::process::exit(1)
    });
    sock.set_broadcast(true).ok();
    sock.set_read_timeout(Some(Duration::from_millis(100))).ok();
    println!(
        "courier: on the Ethernet wire, UDP port {} (answers for a kernel without an address go to {})",
        port, answer
    );
    let state = Arc::new(Mutex::new(Udp {
        sock: sock.try_clone().expect("socket clone"),
        sessions: HashMap::new(),
    }));
    let (job_tx, job_rx) = mpsc::channel::<(String, Job)>();
    // the worker: one job at a time, answers as LINEs kept until GOT
    {
        let state = Arc::clone(&state);
        let brain = Arc::clone(&brain);
        std::thread::spawn(move || {
            for (run, job) in job_rx {
                let o = brain.answer(&job, |body| {
                    let mut st = state.lock().unwrap_or_else(|e| e.into_inner());
                    let Udp { sock, sessions } = &mut *st;
                    let Some(s) = sessions.get_mut(&run) else {
                        return;
                    };
                    s.line_no += 1;
                    let part = s.line_no;
                    let _ = sock.send_to(
                        &datagram(&run, "LINE", job.seq, part, 0, &body),
                        s.answer_to,
                    );
                    s.pending.push(Pending {
                        run: run.clone(),
                        seq: job.seq,
                        part,
                        body,
                        sent: Instant::now(),
                        tries: 1,
                    });
                });
                brain.log("udp", &run, &job, &o);
            }
        });
    }
    let mut buf = [0u8; 2048];
    loop {
        let got = sock.recv_from(&mut buf);
        let mut st = state.lock().unwrap_or_else(|e| e.into_inner());
        // resend LINEs not yet GOT (300 ms), give up on one after 10 s
        {
            let Udp { sock, sessions } = &mut *st;
            for s in sessions.values_mut() {
                for p in s.pending.iter_mut() {
                    if p.sent.elapsed() >= Duration::from_millis(300) {
                        let _ = sock.send_to(
                            &datagram(&p.run, "LINE", p.seq, p.part, 0, &p.body),
                            s.answer_to,
                        );
                        p.sent = Instant::now();
                        p.tries += 1;
                    }
                }
                let before = s.pending.len();
                s.pending.retain(|p| p.tries < 34);
                if s.pending.len() != before {
                    println!("courier: udp: a line was never heard back after 10 s; dropped");
                }
            }
        }
        let Ok((n, from)) = got else {
            continue;
        };
        let text = String::from_utf8_lossy(&buf[..n]).to_string();
        let Some((head, body)) = text.split_once('\n') else {
            continue;
        };
        let Some(rest) = head.strip_prefix(HEAD) else {
            continue;
        };
        let run = rest.split(' ').next().unwrap_or("").to_string();
        if run.len() != 16 || !run.bytes().all(|b| b.is_ascii_hexdigit()) {
            continue;
        }
        let kind = head
            .split(' ')
            .find_map(|f| f.strip_prefix("kind="))
            .unwrap_or("");
        let seq = field(head, "seq=");
        let part = field(head, "part=");
        let total = head
            .split(' ')
            .find_map(|f| f.strip_prefix("part="))
            .and_then(|v| v.split('/').nth(1))
            .and_then(|v| v.parse().ok())
            .unwrap_or(0u32);
        let bytes = field(head, "bytes=");
        let answer_to = if from.ip().is_unspecified() {
            answer
        } else {
            from
        };
        let Udp { sock, sessions } = &mut *st;
        let fresh = !sessions.contains_key(&run);
        let s = sessions.entry(run.clone()).or_insert_with(|| Session {
            answer_to,
            job_parts: HashMap::new(),
            done_seq: 0,
            pending: Vec::new(),
            line_no: 0,
        });
        s.answer_to = answer_to;
        if fresh {
            println!(
                "courier: udp: hello from a kernel, run {}, answers to {}",
                run, answer_to
            );
        }
        match kind {
            "HELLO" => {
                let _ = sock.send_to(&datagram(&run, "HI", 0, 0, 0, ""), answer_to);
            }
            "GOT" => {
                s.pending.retain(|p| !(p.seq == seq && p.part == part));
            }
            "JOB" => {
                if seq <= s.done_seq || total == 0 || part == 0 || part > total || total > 8 {
                    let _ = sock.send_to(&datagram(&run, "ACK", seq, 0, 0, ""), answer_to);
                } else {
                    let e = s
                        .job_parts
                        .entry(seq)
                        .or_insert_with(|| (total, bytes, vec![None; total as usize]));
                    e.2[(part - 1) as usize] = Some(body.to_string());
                    if e.2.iter().all(|p| p.is_some()) {
                        let line: String =
                            e.2.iter().map(|p| p.clone().unwrap_or_default()).collect();
                        let whole = line.len() as u32 == e.1;
                        s.job_parts.remove(&seq);
                        if whole {
                            let _ = sock.send_to(&datagram(&run, "ACK", seq, 0, 0, ""), answer_to);
                            s.done_seq = seq;
                            s.pending.clear();
                            s.line_no = 0;
                            match Job::parse(&line) {
                                Some(job) => {
                                    let _ = job_tx.send((run.clone(), job));
                                }
                                None => println!(
                                    "courier: udp: a job line without its parts was dropped"
                                ),
                            }
                        } else {
                            println!("courier: udp: a job arrived with the wrong length; waiting for it again");
                        }
                    }
                }
            }
            _ => {}
        }
    }
}

fn main() {
    let a = parse_args();
    std::fs::create_dir_all(&a.work).unwrap_or_else(|e| {
        eprintln!("allen-courier: work dir {}: {}", a.work.display(), e);
        std::process::exit(1)
    });
    let brain = Arc::new(Brain {
        model: a.model.clone(),
        max_tokens: a.max_tokens,
        say_max: a.say_max,
        busy_wait: Duration::from_secs(a.busy_wait),
        work: a.work.clone(),
        log_dir: a.log_dir.clone(),
        lock: Mutex::new(()),
    });
    println!(
        "courier: model at {}, learning corpus in {}",
        a.model,
        a.log_dir.display()
    );
    let mut threads = Vec::new();
    if let Some(port) = a.udp {
        let b = Arc::clone(&brain);
        let answer = a.udp_answer;
        threads.push(std::thread::spawn(move || udp_wire(port, answer, b)));
    }
    if let Some(path) = a.socket.clone() {
        let b = Arc::clone(&brain);
        threads.push(std::thread::spawn(move || serial_wire(path, b)));
    }
    for t in threads {
        let _ = t.join();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn words_on_the_wire() {
        assert!(is_word("HELLO", "HELLO"));
        assert!(is_word("HELLO allen", "HELLO"));
        assert!(is_word("HI allen", "HI"));
        assert!(!is_word("HISTORY lesson", "HI"));
        assert!(!is_word("HELLOallen", "HELLO"));
        assert!(!is_word("ELLO allen", "HELLO"));
    }

    #[test]
    fn short_answers_pass_whole_with_whitespace_folded() {
        assert_eq!(
            for_allen("Rain falls.\n\n  Soft.\t", 150),
            "Rain falls. Soft."
        );
    }

    #[test]
    fn long_answers_are_cut_at_a_sentence_end() {
        let text =
            "One sentence here. Two sentences here! A third that runs long and over the line";
        let said = for_allen(text, 45);
        assert_eq!(said, "One sentence here. Two sentences here!");
        assert!(said.chars().count() <= 45);
    }

    #[test]
    fn a_sentence_end_too_early_is_not_used() {
        let text = "Hi. Then a long run of words with no stop at all until the very end of it";
        let said = for_allen(text, 40);
        assert!(said.ends_with("..."), "{said}");
        assert!(said.chars().count() <= 40);
        assert!(said.starts_with("Hi. Then a long run"));
    }

    #[test]
    fn what_was_already_said_in_parts_is_kept() {
        // 20 chars went out as parts; the cut may not land before them
        let text =
            "Short one. Then a long run of words with no stop at all until the very end of it all";
        let said = for_allen_keep(text, 40, 20);
        assert!(said.starts_with("Short one. Then a lo"), "{said}");
        assert!(said.chars().count() <= 40);
    }

    #[test]
    fn json_strings_round_trip() {
        let s = "say \"hi\"\\ now\n\ttab é 😀";
        let lit = json_str(s);
        let (back, end) = json_read_str(lit.as_bytes(), 0).unwrap();
        assert_eq!(back, s);
        assert_eq!(end, lit.len());
        let (t, _) = json_read_str(b"\"a\\u00e9\\ud83d\\ude00\\n\"", 0).unwrap();
        assert_eq!(t, "aé😀\n");
        assert!(json_read_str(b"\"open", 0).is_none());
    }

    #[test]
    fn the_assistant_text_is_found_in_a_chat_completion() {
        let body = br#"{"id":"x","choices":[{"index":0,"message":{"role":"assistant","content":"Rain on the roof.\nQuiet."},"finish_reason":"stop"}],"usage":{"content":1}}"#;
        assert_eq!(content_of(body).unwrap(), "Rain on the roof.\nQuiet.");
        assert!(content_of(br#"{"error":"no"}"#).is_none());
        let delta = br#"{"choices":[{"delta":{"content":"Rain"}}]}"#;
        assert_eq!(json_field(delta, "content").unwrap(), "Rain");
    }

    #[test]
    fn chunked_bodies_are_joined() {
        let b = b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        assert_eq!(dechunk(b), b"hello world");
    }

    #[test]
    fn a_job_line_carries_the_memory_fields() {
        let j = Job::parse("JOB 3 chat You are Nova.\thow are you\tself=Nova\tname=Drake\tstarts=20\tfact my car is red\tnote asked: hi / said: Hello")
            .unwrap();
        assert_eq!(j.seq, 3);
        assert_eq!(j.want, "chat");
        assert_eq!(j.ask, "how are you");
        assert_eq!(j.memory.len(), 5);
        let s = j.system();
        assert!(s.contains("Your name is Nova."));
        assert!(s.contains("called Drake"));
        assert!(s.contains("You were taught: my car is red."));
        assert!(s.contains("A note you keep: asked: hi / said: Hello"));
        assert!(s.starts_with(RULES));
        assert!(Job::parse("JOB 3").is_none());
        // the old shape, without memory, still parses
        let j = Job::parse("JOB 1 make Do it.\twrite a haiku").unwrap();
        assert!(j.memory.is_empty());
        assert_eq!(j.ask, "write a haiku");
    }

    #[test]
    fn datagram_heads_round_trip() {
        let d = datagram("00000000deadbeef", "LINE", 4, 2, 0, "SAY 4 2/0 words");
        let t = String::from_utf8(d).unwrap();
        let (head, body) = t.split_once('\n').unwrap();
        assert_eq!(field(head, "seq="), 4);
        assert_eq!(field(head, "part="), 2);
        assert_eq!(field(head, "bytes="), 15);
        assert_eq!(body, "SAY 4 2/0 words");
        assert!(head.starts_with("ALLEN-COURIER v=1 run=00000000deadbeef kind=LINE"));
    }

    #[test]
    fn a_busy_server_is_asked_again_with_growing_waits() {
        let ms: Vec<u128> = (1..=6)
            .map(|n| backoff(n, "HTTP/1.1 503\r\n\r\n").as_millis())
            .collect();
        assert_eq!(ms, vec![250, 500, 1000, 1000, 1000, 1000]);
        // the server names a wait: used when longer, never past 4 s
        assert_eq!(
            backoff(1, "HTTP/1.1 503\r\nRetry-After: 3\r\n\r\n").as_millis(),
            3000
        );
        assert_eq!(
            backoff(1, "HTTP/1.1 503\r\nretry-after: 30\r\n\r\n").as_millis(),
            4000
        );
    }

    #[test]
    fn dates_are_civil() {
        let (_, s) = utc_now();
        assert_eq!(s.len(), 20);
        assert!(s.starts_with("20"));
    }
}

#[cfg(test)]
mod plain_tests {
    use super::*;

    #[test]
    fn typographic_marks_become_plain_ones() {
        assert_eq!(for_allen("rhythm— earth", 150), "rhythm, earth");
        assert_eq!(
            plain("‘quoted’ “twice” … café naïve Ærø"),
            "'quoted' \"twice\" ... cafe naive AEro"
        );
        assert_eq!(for_allen("rain 😀 falls", 150), "rain falls");
        assert!(plain("Łódź – 3 × 4").is_ascii());
    }

    #[test]
    fn a_cut_answer_stays_within_the_room_with_its_mark() {
        let said = for_allen(
            "no stops at all in this long run of words that goes on and on and on",
            30,
        );
        assert!(said.chars().count() <= 30, "{said}");
        assert!(said.ends_with("..."), "{said}");
        assert!(said.is_ascii());
    }
}
