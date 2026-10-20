//! The language model Allen writes contracts with: any endpoint speaking the
//! OpenAI chat shape (`POST /v1/chat/completions`: Omega serve, Ollama, LM
//! Studio, llama.cpp, OpenAI and most providers) or Anthropic's Messages API.
//! The user picks it in the wizard (L267), which writes `llm.conf`:
//!
//! ```text
//! kind openai            # openai | anthropic
//! base_url http://127.0.0.1:18091
//! model auto             # auto: the first id /v1/models lists
//! key_env OPENAI_API_KEY # the NAME of the variable holding the key, never the key
//! label Allen's own model pair (27B + 30B, working together)
//! ```
//!
//! Plain `http://` goes over the standard library. `https://` needs TLS, and
//! this crate does not carry its own TLS: it runs the system `curl` with the
//! request on stdin and the key in a header file it writes with mode 0600 and
//! deletes after (never on the command line). Encryption is never hand-rolled.

use serde_json::{json, Value};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    OpenAi,
    Anthropic,
}

#[derive(Debug, Clone)]
pub struct Endpoint {
    pub kind: Kind,
    pub base_url: String,
    /// `auto` until resolved by [`Endpoint::resolve_model`].
    pub model: String,
    pub key_env: Option<String>,
    pub label: String,
}

/// Where Allen looks for a model already running on this computer, in order.
/// The wormhole pair (L190) is added in front by its port note when it exists.
pub const LOCAL_CANDIDATES: &[(&str, &str)] = &[
    ("http://127.0.0.1:18091", "Omega serve on this computer"),
    ("http://127.0.0.1:11434", "Ollama"),
    ("http://127.0.0.1:1234", "LM Studio"),
    ("http://127.0.0.1:8080", "llama.cpp server"),
];

impl Endpoint {
    pub fn parse_conf(src: &str) -> Result<Endpoint, String> {
        let mut e = Endpoint {
            kind: Kind::OpenAi,
            base_url: String::new(),
            model: "auto".into(),
            key_env: None,
            label: String::new(),
        };
        for (n, line) in src.lines().enumerate() {
            let line = line.split('#').next().unwrap_or("").trim();
            if line.is_empty() {
                continue;
            }
            let (k, v) = line
                .split_once(' ')
                .map(|(k, v)| (k, v.trim()))
                .unwrap_or((line, ""));
            match k {
                "kind" => {
                    e.kind = match v {
                        "openai" => Kind::OpenAi,
                        "anthropic" => Kind::Anthropic,
                        _ => {
                            return Err(format!(
                                "llm.conf line {}: kind is openai or anthropic",
                                n + 1
                            ))
                        }
                    }
                }
                "base_url" => e.base_url = v.trim_end_matches('/').to_string(),
                "model" => e.model = v.to_string(),
                "key_env" => e.key_env = Some(v.to_string()),
                "label" => e.label = v.to_string(),
                _ => return Err(format!("llm.conf line {}: unknown key {k}", n + 1)),
            }
        }
        if !(e.base_url.starts_with("http://") || e.base_url.starts_with("https://")) {
            return Err("llm.conf: base_url must start with http:// or https://".into());
        }
        if e.label.is_empty() {
            e.label = e.base_url.clone();
        }
        Ok(e)
    }

    /// Where the model runs, for the attempt log: `local` or `cloud`.
    pub fn place(&self) -> &'static str {
        let host = self.base_url.split("://").nth(1).unwrap_or("");
        if host.starts_with("127.") || host.starts_with("localhost") || host.starts_with("[::1]") {
            "local"
        } else {
            "cloud"
        }
    }

    fn key(&self) -> Result<Option<String>, String> {
        match &self.key_env {
            None => Ok(None),
            Some(n) => std::env::var(n)
                .map(Some)
                .map_err(|_| format!("the key variable {n} is not set")),
        }
    }

    fn headers(&self) -> Result<Vec<(String, String)>, String> {
        let mut h = vec![("Content-Type".into(), "application/json".into())];
        if let Some(k) = self.key()? {
            match self.kind {
                Kind::OpenAi => h.push(("Authorization".into(), format!("Bearer {k}"))),
                Kind::Anthropic => h.push(("x-api-key".into(), k)),
            }
        }
        if self.kind == Kind::Anthropic {
            h.push(("anthropic-version".into(), "2023-06-01".into()));
        }
        Ok(h)
    }

    /// Replace `auto` with the first model the endpoint lists.
    pub fn resolve_model(&mut self) -> Result<(), String> {
        if self.model != "auto" {
            return Ok(());
        }
        let (code, body) = request(self, "GET", "/v1/models", None)?;
        if code != 200 {
            return Err(format!("{}/v1/models answered HTTP {code}", self.base_url));
        }
        let v: Value = serde_json::from_slice(&body).map_err(|e| format!("/v1/models: {e}"))?;
        self.model = v["data"][0]["id"]
            .as_str()
            .ok_or("/v1/models listed no model")?
            .to_string();
        Ok(())
    }

    /// The request body for one chat turn.
    pub fn chat_body(&self, system: &str, user: &str, max_tokens: u32) -> Value {
        match self.kind {
            Kind::OpenAi => json!({
                "model": self.model,
                "messages": [
                    {"role": "system", "content": system},
                    {"role": "user", "content": user}
                ],
                "max_tokens": max_tokens,
                "temperature": 0,
                "stream": false
            }),
            Kind::Anthropic => json!({
                "model": self.model,
                "system": system,
                "messages": [{"role": "user", "content": user}],
                "max_tokens": max_tokens,
                "temperature": 0
            }),
        }
    }

    /// One chat turn. A busy server (503) is asked again after the
    /// Retry-After it gives (1 s when it gives none), up to `busy_tries` times.
    pub fn chat(
        &self,
        system: &str,
        user: &str,
        max_tokens: u32,
        busy_tries: u32,
    ) -> Result<String, String> {
        let path = match self.kind {
            Kind::OpenAi => "/v1/chat/completions",
            Kind::Anthropic => "/v1/messages",
        };
        let body = self.chat_body(system, user, max_tokens).to_string();
        let mut tries = 0;
        loop {
            let (code, raw) = request(self, "POST", path, Some(&body))?;
            if code == 503 && tries < busy_tries {
                tries += 1;
                std::thread::sleep(Duration::from_secs(1));
                continue;
            }
            if code != 200 {
                let snippet: String = String::from_utf8_lossy(&raw).chars().take(300).collect();
                return Err(format!("the model answered HTTP {code}: {snippet}"));
            }
            return reply_text(self.kind, &raw);
        }
    }
}

/// The assistant's text in a completed reply.
pub fn reply_text(kind: Kind, raw: &[u8]) -> Result<String, String> {
    let v: Value = serde_json::from_slice(raw).map_err(|e| format!("reply is not JSON: {e}"))?;
    let t = match kind {
        Kind::OpenAi => v["choices"][0]["message"]["content"]
            .as_str()
            .map(str::to_string),
        Kind::Anthropic => v["content"].as_array().map(|parts| {
            parts
                .iter()
                .filter(|p| p["type"] == "text")
                .filter_map(|p| p["text"].as_str())
                .collect::<Vec<_>>()
                .join("")
        }),
    };
    t.filter(|t| !t.is_empty())
        .ok_or_else(|| "reply holds no text".into())
}

fn find(h: &[u8], n: &[u8]) -> Option<usize> {
    h.windows(n.len()).position(|w| w == n)
}

fn dechunk(mut b: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    while let Some(eol) = find(b, b"\r\n") {
        let size = std::str::from_utf8(&b[..eol])
            .ok()
            .and_then(|s| usize::from_str_radix(s.split(';').next().unwrap_or("").trim(), 16).ok())
            .unwrap_or(0);
        if size == 0 || eol + 2 + size > b.len() {
            break;
        }
        out.extend_from_slice(&b[eol + 2..eol + 2 + size]);
        b = &b[(eol + 4 + size).min(b.len())..];
    }
    out
}

/// Split a raw HTTP/1.1 response into status and body.
pub fn parse_response(raw: &[u8]) -> Result<(u16, Vec<u8>), String> {
    let end = find(raw, b"\r\n\r\n").ok_or("no HTTP head in the answer")?;
    let head = String::from_utf8_lossy(&raw[..end]);
    let code = head
        .split_whitespace()
        .nth(1)
        .and_then(|c| c.parse().ok())
        .ok_or("no HTTP status")?;
    let body = &raw[end + 4..];
    let chunked = head.lines().any(|l| {
        l.to_ascii_lowercase().starts_with("transfer-encoding:")
            && l.to_ascii_lowercase().contains("chunked")
    });
    Ok((
        code,
        if chunked {
            dechunk(body)
        } else {
            body.to_vec()
        },
    ))
}

fn request(
    e: &Endpoint,
    method: &str,
    path: &str,
    body: Option<&str>,
) -> Result<(u16, Vec<u8>), String> {
    let headers = e.headers()?;
    if let Some(hostport) = e.base_url.strip_prefix("http://") {
        let hostport = hostport.split('/').next().unwrap_or(hostport);
        let addr = if hostport.contains(':') {
            hostport.to_string()
        } else {
            format!("{hostport}:80")
        };
        let mut s =
            TcpStream::connect(&addr).map_err(|err| format!("cannot reach {addr}: {err}"))?;
        s.set_read_timeout(Some(Duration::from_secs(900))).ok();
        let body = body.unwrap_or("");
        let mut req =
            format!("{method} {path} HTTP/1.1\r\nHost: {hostport}\r\nConnection: close\r\n");
        for (k, v) in &headers {
            req.push_str(&format!("{k}: {v}\r\n"));
        }
        req.push_str(&format!("Content-Length: {}\r\n\r\n{body}", body.len()));
        s.write_all(req.as_bytes()).map_err(|err| err.to_string())?;
        let mut raw = Vec::new();
        s.read_to_end(&mut raw)
            .map_err(|err| format!("reading the answer: {err}"))?;
        return parse_response(&raw);
    }
    curl_request(e, method, path, body, &headers)
}

/// HTTPS through the system curl. The headers (with the key) go in a 0600
/// file, the body on stdin; neither appears on a command line.
fn curl_request(
    e: &Endpoint,
    method: &str,
    path: &str,
    body: Option<&str>,
    headers: &[(String, String)],
) -> Result<(u16, Vec<u8>), String> {
    use std::os::unix::fs::OpenOptionsExt;
    let dir = std::env::temp_dir().join(format!("allen-llm-{}", std::process::id()));
    std::fs::create_dir_all(&dir).map_err(|err| err.to_string())?;
    let hfile = dir.join("headers");
    let mut f = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(&hfile)
        .map_err(|err| err.to_string())?;
    for (k, v) in headers {
        writeln!(f, "{k}: {v}").map_err(|err| err.to_string())?;
    }
    drop(f);
    let url = format!("{}{path}", e.base_url);
    let mut cmd = std::process::Command::new("curl");
    cmd.args(["-sS", "-i", "--max-time", "900", "-X", method, "-H"])
        .arg(format!("@{}", hfile.display()))
        .arg("--data-binary")
        .arg("@-")
        .arg(&url)
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped());
    let out = (|| {
        let mut child = cmd.spawn().map_err(|err| format!("curl: {err}"))?;
        child
            .stdin
            .take()
            .ok_or("curl stdin")?
            .write_all(body.unwrap_or("").as_bytes())
            .map_err(|err| err.to_string())?;
        child.wait_with_output().map_err(|err| err.to_string())
    })();
    let _ = std::fs::remove_file(&hfile);
    let _ = std::fs::remove_dir(&dir);
    let out = out?;
    if !out.status.success() {
        return Err(format!(
            "curl {url}: {}",
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    // curl -i may print a 100 Continue head first.
    let mut raw: &[u8] = &out.stdout;
    while raw.starts_with(b"HTTP/1.1 100") || raw.starts_with(b"HTTP/2 100") {
        let end = find(raw, b"\r\n\r\n").ok_or("bad 100 head")?;
        raw = &raw[end + 4..];
    }
    parse_response(raw)
}

/// Models already running on this computer: (base_url, what it is, first model id).
pub fn detect_local(extra: &[(String, String)]) -> Vec<(String, String, String)> {
    let mut found = Vec::new();
    let all = extra.iter().cloned().chain(
        LOCAL_CANDIDATES
            .iter()
            .map(|(u, w)| (u.to_string(), w.to_string())),
    );
    for (url, what) in all {
        let mut e = Endpoint {
            kind: Kind::OpenAi,
            base_url: url.clone(),
            model: "auto".into(),
            key_env: None,
            label: what.clone(),
        };
        if e.resolve_model().is_ok() {
            found.push((url, what, e.model));
        }
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    /// A one-shot OpenAI-shaped server on a free port, answering `answers` in order.
    pub(crate) fn fake_server(
        answers: Vec<(u16, String)>,
    ) -> (String, std::thread::JoinHandle<Vec<String>>) {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let url = format!("http://{}", l.local_addr().unwrap());
        let h = std::thread::spawn(move || {
            let mut seen = Vec::new();
            for (code, body) in answers {
                let (mut s, _) = l.accept().unwrap();
                let mut buf = vec![0u8; 1 << 20];
                let mut got = Vec::new();
                loop {
                    let n = s.read(&mut buf).unwrap();
                    got.extend_from_slice(&buf[..n]);
                    if let Some(e) = find(&got, b"\r\n\r\n") {
                        let head = String::from_utf8_lossy(&got[..e]).to_string();
                        let len: usize = head
                            .lines()
                            .find_map(|l| l.strip_prefix("Content-Length: "))
                            .and_then(|v| v.trim().parse().ok())
                            .unwrap_or(0);
                        if got.len() >= e + 4 + len || n == 0 {
                            break;
                        }
                    }
                }
                seen.push(String::from_utf8_lossy(&got).to_string());
                let resp = format!(
                    "HTTP/1.1 {code} X\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{body}",
                    body.len()
                );
                s.write_all(resp.as_bytes()).unwrap();
            }
            seen
        });
        (url, h)
    }

    pub(crate) fn openai_reply(text: &str) -> String {
        json!({"choices": [{"message": {"role": "assistant", "content": text}}]}).to_string()
    }

    #[test]
    fn conf_is_read_and_checked() {
        let e = Endpoint::parse_conf(
            "kind anthropic\nbase_url https://api.anthropic.com/\nmodel claude-opus-5-5\nkey_env ANTHROPIC_API_KEY\n",
        )
        .unwrap();
        assert_eq!(e.kind, Kind::Anthropic);
        assert_eq!(e.base_url, "https://api.anthropic.com");
        assert_eq!(e.place(), "cloud");
        assert!(Endpoint::parse_conf("kind grok\nbase_url http://x\n").is_err());
        assert!(Endpoint::parse_conf("base_url ftp://x\n").is_err());
        assert_eq!(
            Endpoint::parse_conf("base_url http://127.0.0.1:18091\n")
                .unwrap()
                .place(),
            "local"
        );
    }

    #[test]
    fn both_reply_shapes_are_read() {
        assert_eq!(
            reply_text(Kind::OpenAi, openai_reply("hi").as_bytes()).unwrap(),
            "hi"
        );
        let a =
            json!({"content": [{"type": "text", "text": "he"}, {"type": "text", "text": "llo"}]})
                .to_string();
        assert_eq!(reply_text(Kind::Anthropic, a.as_bytes()).unwrap(), "hello");
        assert!(reply_text(Kind::OpenAi, b"{}").is_err());
    }

    #[test]
    fn chunked_bodies_are_joined() {
        let raw = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n";
        assert_eq!(parse_response(raw).unwrap(), (200, b"abcde".to_vec()));
    }

    #[test]
    fn auto_model_then_chat_with_one_busy_answer() {
        let (url, h) = fake_server(vec![
            (
                200,
                json!({"data": [{"id": "qwen3.8-27b-fp8"}]}).to_string(),
            ),
            (503, "{}".into()),
            (200, openai_reply("devc 1")),
        ]);
        let mut e = Endpoint::parse_conf(&format!("base_url {url}\n")).unwrap();
        e.resolve_model().unwrap();
        assert_eq!(e.model, "qwen3.8-27b-fp8");
        assert_eq!(e.chat("sys", "user", 64, 3).unwrap(), "devc 1");
        let seen = h.join().unwrap();
        assert!(seen[0].starts_with("GET /v1/models "));
        assert!(seen[2].starts_with("POST /v1/chat/completions "));
        assert!(seen[2].contains("\"model\":\"qwen3.8-27b-fp8\""));
        assert!(!seen[2].contains("Authorization"));
    }

    #[test]
    fn a_missing_key_variable_stops_before_any_request() {
        let e = Endpoint::parse_conf(
            "base_url http://127.0.0.1:9\nkey_env ALLEN_TEST_NO_SUCH_KEY_VAR\n",
        )
        .unwrap();
        let err = e.chat("s", "u", 8, 0).unwrap_err();
        assert!(err.contains("ALLEN_TEST_NO_SUCH_KEY_VAR"), "{err}");
    }
}
