//! Ordered request handling. A message is cut into requests at explicit sequence markers
//! ("and then", "and afterwards", ", later", "after that", ";", "and approve ..."). The exam contract says
//! "if the message asks for several steps, give only the first step", so the core decides on
//! the first request that is not a pure acknowledgement, and copies its argument only from it.
//! Sentence breaks alone do not cut ("I do not know what to ask. Show me the options." is
//! one request), except that a leading sentence of fillers ("Ok.") is dropped.

use crate::lexicon::{
    is_in, longest, skip_polite, AND_VERBS, FILLER, FILLER_COMMA, SEQUENCE_ADVERBS, TEXT_FRAMES,
};
use crate::text::{gap, Token};
use alloc::vec::Vec;

/// A request: token range `[start, end)`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Clause {
    /// first token
    pub start: usize,
    /// one past the last token
    pub end: usize,
}

const TRAILING_MARKERS: &[&str] = &["afterwards", "afterward", "later", "next"];

/// Most words between a payload frame and its colon ("Save this as a fact: ...").
const FRAME_REACH: usize = 8;

/// If a store, to-do, preference or writing frame opens at `at` (after courtesy words) and a
/// colon or free dash follows it within [`FRAME_REACH`] words of the same sentence, the
/// payload it introduces: `Some((first, end))`, ending at the next sentence stop or `;`.
pub fn delimited_payload(toks: &[Token], at: usize) -> Option<(usize, usize)> {
    let n = toks.len();
    let lead = skip_polite(toks, at, n);
    let f = longest(toks, lead, TEXT_FRAMES);
    if f == 0 {
        return None;
    }
    let hi = core::cmp::min(n, lead + FRAME_REACH + 1);
    let mut j = lead + 1;
    while j < hi {
        let g = toks[j].gap;
        if g & (gap::STOP | gap::SEMI) != 0 {
            return None;
        }
        if g & (gap::COLON | gap::DASH) != 0 {
            let mut e = j + 1;
            while e < n && toks[e].gap & (gap::STOP | gap::SEMI) == 0 {
                e += 1;
            }
            return Some((j, e));
        }
        j += 1;
    }
    None
}

/// Cut `toks` into requests in order.
///
/// Cuts: `;`; a sequence adverb ("then", "afterwards", "later", "next", ...) right after
/// "and" or after a comma or sentence stop; "after that" in the same positions; and
/// "X and <agent verb>". The last one is not made inside a delimited payload (a frame plus a
/// colon or dash, see [`delimited_payload`]): there the operator is dictating content, and a
/// coordinated verb belongs to it ("Note this: call Ann and cancel the gym" is one note).
/// Explicit sequence words still cut everywhere.
pub fn clauses(toks: &[Token]) -> Vec<Clause> {
    let mut out = Vec::new();
    let n = toks.len();
    let mut s = 0;
    let mut i = 0;
    // current delimited payload [from, to)
    let mut payload: Option<(usize, usize)> = None;
    while i < n {
        let w = toks[i].word.as_str();
        let g = toks[i].gap;
        if i == s || g & (gap::STOP | gap::SEMI) != 0 {
            if let Some(p) = delimited_payload(toks, i) {
                payload = Some(p);
            }
        }
        let in_payload = payload.is_some_and(|(a, b)| a <= i && i < b);
        // ";" always separates
        if i > s && g & gap::SEMI != 0 {
            out.push(Clause { start: s, end: i });
            s = i;
            continue;
        }
        // "and then", ", then", ". Then", "and afterwards", ", later", ...
        if is_in(SEQUENCE_ADVERBS, w) && i > s {
            let prev_and = toks[i - 1].word == "and" && i - 1 > s;
            if prev_and || g & (gap::COMMA | gap::STOP) != 0 {
                let e = if prev_and { i - 1 } else { i };
                out.push(Clause { start: s, end: e });
                s = i + 1;
                i += 1;
                continue;
            }
        }
        // "after that" (", after that", "and after that")
        if w == "after" && i + 1 < n && toks[i + 1].word == "that" && i > s {
            let prev_and = toks[i - 1].word == "and" && i - 1 > s;
            if prev_and || g & (gap::COMMA | gap::STOP) != 0 {
                let e = if prev_and { i - 1 } else { i };
                out.push(Clause { start: s, end: e });
                s = i + 2;
                i += 2;
                continue;
            }
        }
        // "X and approve it": a second agent verb starts a new request, outside a payload
        if w == "and"
            && i > s
            && i + 1 < n
            && !in_payload
            && is_in(AND_VERBS, toks[i + 1].word.as_str())
        {
            out.push(Clause { start: s, end: i });
            s = i + 1;
            i += 1;
            continue;
        }
        i += 1;
    }
    if s < n {
        out.push(Clause { start: s, end: n });
    }
    // trailing sequence markers ("... and approve it afterwards") are not content
    for c in out.iter_mut() {
        while c.end > c.start && is_in(TRAILING_MARKERS, toks[c.end - 1].word.as_str()) {
            c.end -= 1;
        }
    }
    out.retain(|c| c.end > c.start);
    out
}

fn all_filler(toks: &[Token], s: usize, e: usize) -> bool {
    e > s
        && toks[s..e]
            .iter()
            .all(|t| !t.alnum || is_in(FILLER, &t.word))
}

/// The request the decision is about: the first clause that is not only fillers, with any
/// leading filler sentence or filler-comma word removed. Falls back to the first clause.
pub fn primary(toks: &[Token], cl: &[Clause]) -> Option<Clause> {
    let first = *cl.first()?;
    let pick = cl
        .iter()
        .copied()
        .find(|c| !all_filler(toks, c.start, c.end))
        .unwrap_or(first);
    let mut c = pick;
    // drop leading filler sentences ("Ok. Remember ...")
    loop {
        let mut j = c.start + 1;
        while j < c.end && toks[j].gap & gap::STOP == 0 {
            j += 1;
        }
        if j < c.end && all_filler(toks, c.start, j) {
            c.start = j;
        } else {
            break;
        }
    }
    // drop "Ok," / "Well," before more words
    while c.end - c.start > 1
        && is_in(FILLER_COMMA, &toks[c.start].word)
        && toks[c.start + 1].gap & gap::COMMA != 0
    {
        c.start += 1;
    }
    Some(c)
}
