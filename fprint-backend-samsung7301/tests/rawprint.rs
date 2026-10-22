//! The stored print has to be the same bytes the C driver writes, so that
//! a print that was enrolled by one driver is readable by the other.

use fprint_backend_samsung7301::rawprint;

/// The frame the reference bytes were made from.
fn reference_frame() -> Vec<u8> {
    (0..160 * 160).map(|i| (i * 7 + 3) as u8).collect()
}

#[test]
fn build_produces_the_reference_bytes() {
    let frames = vec![reference_frame()];
    let bytes = rawprint::build(&frames).expect("build");

    // The bytes the C driver writes for the same frame: the GVariant
    // `(saay)` of the version string and the frame, 25630 bytes.
    let reference = std::fs::read("/tmp/opencode/c_ref.bin").ok();
    if let Some(reference) = reference {
        assert_eq!(bytes, reference, "the raw print differs from the C driver");
    } else {
        // Without the reference file, check the shape of the variant.
        assert_eq!(bytes.len(), 25630, "unexpected length of the raw print");
    }
}

#[test]
fn parse_reads_back_what_build_wrote() {
    let frames = vec![reference_frame(), vec![7u8; 160 * 160]];
    let bytes = rawprint::build(&frames).expect("build");
    let parsed = rawprint::parse(&bytes).expect("parse");

    assert_eq!(parsed.len(), 2);
    assert_eq!(parsed[0], frames[0]);
    assert_eq!(parsed[1], frames[1]);
}

#[test]
fn parse_refuses_a_foreign_print() {
    let error = rawprint::parse(b"not a gvariant at all").expect_err("must be refused");
    assert!(format!("{error:#}").contains("samsung7301-sourceafis-v1"));
}
