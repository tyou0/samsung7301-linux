//! Client for the SourceAFIS matcher helper.
//!
//! The frames of this sensor do not carry enough minutiae for the
//! NBIS/bozorth3 path, so the matching is done by the real SourceAFIS
//! matcher.  The helper is a separate program that reads one blob from
//! stdin and writes one result line to stdout; it is started without a
//! shell and without an argument.
//!
//! The wire format is:
//!
//! ```text
//! stdin:  "S731AF01" | u32le probe_count | u32le ref_count
//!                     | probe_count frames | ref_count frames
//! stdout: "OK <quality_max> <best_cross> <selected_count> <i0> <i1> <i2> <i3>"
//! ```
//!
//! Every failure prints nothing on stdout and exits with a non zero status.

use std::io::Write;
use std::process::{Command, Stdio};

/// The absolute path of the helper.  It is not taken from the environment.
pub const HELPER_PATH: &str = "/usr/local/libexec/libfprint-samsung7301-afis";

/// The magic that starts every request.
const MAGIC: &[u8; 8] = b"S731AF01";

/// The number of frames the helper accepts.
const MAX_FRAMES: usize = 32;

/// SourceAFIS's own match threshold.  It is not lowered anywhere.
pub const MATCH_THRESHOLD: f64 = 40.0;

/// What the helper reported.
#[derive(Clone, Debug)]
pub struct MatchOutcome {
    /// The best capture strength over the probes.
    pub quality_max: f64,
    /// The best identity score of a probe against the references.
    pub best_cross: f64,
    /// The indices of the probes the helper selected.
    pub selected: Vec<usize>,
}

impl MatchOutcome {
    /// Whether the capture was strong enough to be usable.
    pub fn capture_is_strong(&self) -> bool {
        self.quality_max >= MATCH_THRESHOLD
    }

    /// Whether a probe matched the references.
    pub fn matched(&self) -> bool {
        self.best_cross >= MATCH_THRESHOLD
    }
}

/// Run the helper over `probes` and `refs`.
///
/// Both are lists of native frames of [`samsung7301::protocol::IMAGE_LEN`]
/// bytes.  The helper keeps every template in RAM only; nothing is
/// serialized and no file is read or written.
pub fn match_frames(probes: &[Vec<u8>], refs: &[Vec<u8>]) -> anyhow::Result<MatchOutcome> {
    anyhow::ensure!(
        probes.len() <= MAX_FRAMES && refs.len() <= MAX_FRAMES,
        "the matcher helper accepts at most {MAX_FRAMES} frames of each kind"
    );
    anyhow::ensure!(!probes.is_empty(), "no probe frame");

    let mut input = Vec::with_capacity(16 + (probes.len() + refs.len()) * 25_600);
    input.extend_from_slice(MAGIC);
    input.extend_from_slice(&(probes.len() as u32).to_le_bytes());
    input.extend_from_slice(&(refs.len() as u32).to_le_bytes());
    for frame in probes.iter().chain(refs.iter()) {
        input.extend_from_slice(frame);
    }

    let mut child = Command::new(HELPER_PATH)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| anyhow::anyhow!("starting the matcher helper: {e}"))?;

    child
        .stdin
        .as_mut()
        .expect("stdin is piped")
        .write_all(&input)
        .map_err(|e| anyhow::anyhow!("writing to the matcher helper: {e}"))?;

    let out = child
        .wait_with_output()
        .map_err(|e| anyhow::anyhow!("waiting for the matcher helper: {e}"))?;

    anyhow::ensure!(
        out.status.success(),
        "the matcher helper failed with {}",
        out.status
    );

    parse_output(&out.stdout)
}

/// Parse the single result line of the helper.
fn parse_output(stdout: &[u8]) -> anyhow::Result<MatchOutcome> {
    let text = std::str::from_utf8(stdout)?.trim();
    anyhow::ensure!(!text.is_empty(), "the matcher helper printed nothing");

    let mut fields = text.split_whitespace();
    anyhow::ensure!(fields.next() == Some("OK"), "the matcher helper printed {text:?}");

    let quality_max: f64 = fields
        .next()
        .ok_or_else(|| anyhow::anyhow!("the matcher helper printed no quality"))?
        .parse()?;
    let best_cross: f64 = fields
        .next()
        .ok_or_else(|| anyhow::anyhow!("the matcher helper printed no cross score"))?
        .parse()?;
    let count: usize = fields
        .next()
        .ok_or_else(|| anyhow::anyhow!("the matcher helper printed no frame count"))?
        .parse()?;

    let mut selected = Vec::with_capacity(count);
    for _ in 0..count {
        let index: i64 = fields
            .next()
            .ok_or_else(|| anyhow::anyhow!("the matcher helper printed too few indices"))?
            .parse()?;
        anyhow::ensure!(index >= 0, "the matcher helper selected the frame {index}");
        selected.push(index as usize);
    }

    Ok(MatchOutcome {
        quality_max,
        best_cross,
        selected,
    })
}
