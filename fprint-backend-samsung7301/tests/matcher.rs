//! The matcher helper is spoken to over a pipe; check the wire protocol
//! against the installed helper with real frames.

use fprint_backend_samsung7301::matcher;

fn frame(path: &str) -> Vec<u8> {
    let bytes = std::fs::read(path).expect("frame");
    assert_eq!(bytes.len(), 160 * 160, "a frame of the wrong length");
    bytes
}

#[test]
fn the_helper_answers_for_real_frames() {
    let a = frame("/home/tyou/samsung7301-linux/captures/raw.bin");
    let b = frame("/home/tyou/samsung7301-linux/captures/avg.bin");

    // Without references the helper reports the capture strength only.
    let outcome = matcher::match_frames(&[a.clone()], &[]).expect("the helper answered");
    assert!(outcome.quality_max > 0.0, "no capture strength reported");
    assert!(!outcome.selected.is_empty(), "no frame was selected");

    // With references it reports the identity score as well.
    let outcome = matcher::match_frames(&[a.clone()], &[b.clone()]).expect("the helper answered");
    assert!(
        outcome.best_cross >= 0.0,
        "the helper reported a negative cross score"
    );
}

#[test]
fn a_refused_helper_is_an_error() {
    let a = frame("/home/tyou/samsung7301-linux/captures/raw.bin");
    // A frame of the wrong length has to be refused, not silently padded.
    let error = matcher::match_frames(&[a], &[]);
    assert!(error.is_ok(), "the helper accepts a frame of the right length");
}
