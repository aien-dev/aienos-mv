//! allen-courier: the far end of Allen's courier wire (native/kernel/svc/courier.h).
//!
//! Allen, inside the AIENOS kernel, writes one job line per question he cannot
//! answer by heart ("JOB <seq> <want> <instruction>\t<line>"). This relay sits
//! on the other end of his serial wire (in QEMU: the Unix socket behind
//! `-serial unix:...,server=on,wait=off`), sends the job to a language-model
//! server speaking the OpenAI chat shape (`POST /v1/chat/completions`), and
//! writes the answer back as one line: "SAY <seq> <text>". The whole answer is
//! kept in a work folder; Allen is sent only as much as his window can show,
//! cut at a sentence end. Standard library only: hand-built HTTP and JSON.
//!
//! The handshake: "HELLO <who>" is a call, answered with "HI <who>", which is
//! never answered. This relay calls every 2 s until it hears either, and
//! answers every call it hears; the kernel does the same from its side, so the
//! two meet whichever was up first (the kernel's serial set-up clears its FIFOs
//! and can eat a call sent too early; its own call then draws our answer).
//!
//! Lengths and timings go to stdout; the words never do.
use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

struct Args {
    socket: PathBuf,
    model: String, // host:port of the chat server
    work: PathBuf,
    max_tokens: u32,
    say_max: usize,
}

fn usage() -> ! {
    eprintln!(
        "usage: allen-courier --socket PATH [--model HOST:PORT] [--work DIR] [--max-tokens N] [--say-max N]"
    );
    std::process::exit(2)
}

fn parse_args() -> Args {
    let mut a = Args {
        socket: PathBuf::new(),
        model: "127.0.0.1:18091".into(),
        work: std::env::temp_dir().join("allen-courier"),
        max_tokens: 220,
        say_max: 150,
    };
    let mut it = std::env::args().skip(1);
    while let Some(k) = it.next() {
        let v = it.next().unwrap_or_else(|| usage());
        match k.as_str() {
            "--socket" => a.socket = PathBuf::from(v),
            "--model" => a.model = v,
            "--work" => a.work = PathBuf::from(v),
            "--max-tokens" => a.max_tokens = v.parse().unwrap_or_else(|_| usage()),
            "--say-max" => a.say_max = v.parse().unwrap_or_else(|_| usage()),
            _ => usage(),
        }
    }
    if a.socket.as_os_str().is_empty() {
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

/// The assistant text in a chat completion: the first "content" string after "message".
fn content_of(body: &[u8]) -> Option<String> {
    let needle = b"\"content\"";
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

fn find(h: &[u8], n: &[u8], from: usize) -> Option<usize> {
    if n.is_empty() || h.len() < n.len() {
        return None;
    }
    (from..=h.len() - n.len()).find(|&i| &h[i..i + n.len()] == n)
}

/// POSTs the chat request and returns (status, body).
fn chat(model: &str, system: &str, user: &str, max_tokens: u32) -> std::io::Result<(u16, Vec<u8>)> {
    let body = format!(
        "{{\"messages\":[{{\"role\":\"system\",\"content\":{}}},{{\"role\":\"user\",\"content\":{}}}],\
         \"max_tokens\":{},\"temperature\":0.4,\"stream\":false}}",
        json_str(system),
        json_str(user),
        max_tokens
    );
    let mut s = TcpStream::connect(model)?;
    s.set_read_timeout(Some(Duration::from_secs(120)))?;
    write!(
        s,
        "POST /v1/chat/completions HTTP/1.1\r\nHost: {}\r\nContent-Type: application/json\r\n\
         Content-Length: {}\r\nConnection: close\r\n\r\n{}",
        model,
        body.len(),
        body
    )?;
    let mut raw = Vec::new();
    s.read_to_end(&mut raw)?;
    let head_end =
        find(&raw, b"\r\n\r\n", 0).ok_or_else(|| std::io::Error::other("no http head"))?;
    let head = String::from_utf8_lossy(&raw[..head_end]).to_string();
    let status: u16 = head
        .split_whitespace()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    let mut body = raw[head_end + 4..].to_vec();
    if head
        .to_ascii_lowercase()
        .contains("transfer-encoding: chunked")
    {
        body = dechunk(&body);
    }
    Ok((status, body))
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

/// What Allen is handed: plain letters, whitespace folded, at most max chars,
/// cut at the last sentence end that fits (else at a word, with "..." in the
/// room left for it), with no newlines at all.
fn for_allen(text: &str, max: usize) -> String {
    let folded: String = plain(text).split_whitespace().collect::<Vec<_>>().join(" ");
    if folded.chars().count() <= max {
        return folded;
    }
    let head: String = folded.chars().take(max).collect();
    if let Some(cut) = head
        .rmatch_indices(['.', '!', '?'])
        .map(|(i, m)| i + m.len())
        .find(|&i| i >= max / 3)
    {
        return head[..cut].trim_end().to_string();
    }
    let room: String = head.chars().take(max.saturating_sub(3)).collect();
    let cut = room.rfind(' ').unwrap_or(room.len());
    format!("{}...", room[..cut].trim_end())
}

/// True when the line is the word alone or the word followed by a space.
fn is_word(line: &str, word: &str) -> bool {
    line == word || line.strip_prefix(word).is_some_and(|r| r.starts_with(' '))
}

fn connect(path: &Path) -> UnixStream {
    let t0 = Instant::now();
    loop {
        match UnixStream::connect(path) {
            Ok(s) => return s,
            Err(e) if t0.elapsed() < Duration::from_secs(120) => {
                let _ = e;
                std::thread::sleep(Duration::from_millis(200));
            }
            Err(e) => {
                eprintln!("allen-courier: no wire at {}: {}", path.display(), e);
                std::process::exit(1);
            }
        }
    }
}

fn main() {
    let a = parse_args();
    std::fs::create_dir_all(&a.work).unwrap_or_else(|e| {
        eprintln!("allen-courier: work dir {}: {}", a.work.display(), e);
        std::process::exit(1)
    });
    let wire = connect(&a.socket);
    wire.set_read_timeout(Some(Duration::from_millis(500))).ok();
    let mut tx = wire.try_clone().expect("wire clone");
    let mut rx = BufReader::new(wire);
    println!(
        "courier: on the wire {}, model at {}",
        a.socket.display(),
        a.model
    );
    let _ = tx.write_all(b"HELLO courier\n");
    let mut hello = false;
    let mut last_hello = Instant::now();
    let mut line = Vec::new();
    loop {
        line.clear();
        match rx.read_until(b'\n', &mut line) {
            Ok(0) => {
                println!("courier: the wire closed");
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
                println!("courier: wire error: {}", e);
                return;
            }
        }
        if !line.ends_with(b"\n") {
            continue; // a part line at the end of the wire
        }
        let text = String::from_utf8_lossy(&line)
            .trim_end_matches(['\r', '\n'])
            .to_string();
        if is_word(&text, "HELLO") || is_word(&text, "HI") {
            if !hello {
                println!("courier: hello from the far side");
            }
            hello = true;
            if is_word(&text, "HELLO") {
                let _ = tx.write_all(b"HI courier\n");
            }
            continue;
        }
        let Some(rest) = text.strip_prefix("JOB ") else {
            continue;
        };
        let mut parts = rest.splitn(3, ' ');
        let (Some(seq), Some(want), Some(body)) = (parts.next(), parts.next(), parts.next()) else {
            println!("courier: a job line without its parts was dropped");
            continue;
        };
        let (instruction, ask) = body.split_once('\t').unwrap_or((body, ""));
        let t0 = Instant::now();
        let reply = match chat(&a.model, instruction, ask, a.max_tokens) {
            Ok((200, b)) => content_of(&b).unwrap_or_default(),
            Ok((code, _)) => {
                println!(
                    "courier: job {} ({}): the model answered {}",
                    seq, want, code
                );
                String::new()
            }
            Err(e) => {
                println!("courier: job {} ({}): no model: {}", seq, want, e);
                String::new()
            }
        };
        let ms = t0.elapsed().as_millis();
        let said = if reply.trim().is_empty() {
            "The far side had nothing to say.".to_string()
        } else {
            for_allen(&reply, a.say_max)
        };
        let _ = std::fs::write(a.work.join(format!("{}.txt", seq)), &reply);
        if let Err(e) = tx.write_all(format!("SAY {} {}\n", seq, said).as_bytes()) {
            println!("courier: job {}: could not answer: {}", seq, e);
            return;
        }
        println!(
            "courier: job {} ({}, {} chars asked, {} in the instruction) -> {} chars, {} said, in {} ms",
            seq,
            want,
            ask.chars().count(),
            instruction.chars().count(),
            reply.chars().count(),
            said.chars().count(),
            ms
        );
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
        // the only full stop is in the first third: cut at a word instead, and say so
        let text = "Hi. Then a long run of words with no stop at all until the very end of it";
        let said = for_allen(text, 40);
        assert!(said.ends_with("..."), "{said}");
        assert!(said.chars().count() <= 40);
        assert!(said.starts_with("Hi. Then a long run"));
    }

    #[test]
    fn json_strings_round_trip() {
        let s = "say \"hi\"\\ now\n\ttab é 😀";
        let lit = json_str(s);
        let (back, end) = json_read_str(lit.as_bytes(), 0).unwrap();
        assert_eq!(back, s);
        assert_eq!(end, lit.len());
        // escapes as a server writes them
        let (t, _) = json_read_str(b"\"a\\u00e9\\ud83d\\ude00\\n\"", 0).unwrap();
        assert_eq!(t, "aé😀\n");
        assert!(json_read_str(b"\"open", 0).is_none());
    }

    #[test]
    fn the_assistant_text_is_found_in_a_chat_completion() {
        let body = br#"{"id":"x","choices":[{"index":0,"message":{"role":"assistant","content":"Rain on the roof.\nQuiet."},"finish_reason":"stop"}],"usage":{"content":1}}"#;
        assert_eq!(content_of(body).unwrap(), "Rain on the roof.\nQuiet.");
        assert!(content_of(br#"{"error":"no"}"#).is_none());
    }

    #[test]
    fn chunked_bodies_are_joined() {
        let b = b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        assert_eq!(dechunk(b), b"hello world");
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
