//! The stored print: the version string and the native frames.
//!
//! The print is a `FPI_PRINT_RAW` print whose payload is the GVariant
//! `(saay)` that the C driver writes: the version string and the selected
//! native frames.  The same layout is used here, so a print that was
//! enrolled by one driver is readable by the other.
//!
//! The frames are kept as they came out of the sensor.  The matcher
//! re-derives the template from them every time, so nothing about the
//! matching is frozen into the print.

use glib::Variant;

/// The version of the raw print layout.
pub const RAW_PRINT_VERSION: &str = "samsung7301-sourceafis-v1";

/// The length of one native frame.
pub const FRAME_SIZE: usize = 160 * 160;

/// Build the payload of the print from the selected frames.
pub fn build(frames: &[Vec<u8>]) -> anyhow::Result<Vec<u8>> {
    for frame in frames {
        anyhow::ensure!(
            frame.len() == FRAME_SIZE,
            "a frame of {} bytes instead of {FRAME_SIZE}",
            frame.len()
        );
    }

    let element_type = glib::VariantTy::new("ay").expect("ay is a type");
    let array = Variant::array_from_iter_with_type(
        &element_type,
        frames.iter().map(|frame| {
            let bytes = glib::Bytes::from_owned(frame.clone());
            Variant::from_bytes_with_type(&bytes, &element_type)
        }),
    );

    let tuple = Variant::tuple_from_iter([
        Variant::from(RAW_PRINT_VERSION.to_string()),
        array,
    ]);

    Ok(tuple.data().to_vec())
}

/// Read the frames back out of the payload of a print.
pub fn parse(data: &[u8]) -> anyhow::Result<Vec<Vec<u8>>> {
    let bytes = glib::Bytes::from_owned(data.to_vec());
    let expected = glib::VariantTy::new("(saay)").expect("(saay) is a type");
    let variant = Variant::from_bytes_with_type(&bytes, &expected);

    anyhow::ensure!(
        variant.type_() == expected,
        "the stored print is not a \"{RAW_PRINT_VERSION}\" print"
    );
    anyhow::ensure!(variant.n_children() == 2, "the stored print has a wrong shape");

    let version_value = variant.child_value(0);
    let version = version_value.str().unwrap_or_default();
    anyhow::ensure!(
        version == RAW_PRINT_VERSION,
        "the stored print has the version \"{version}\" instead of \"{RAW_PRINT_VERSION}\", \
         please re-enrol this finger"
    );

    let array = variant.child_value(1);
    let mut frames = Vec::with_capacity(array.n_children());
    for i in 0..array.n_children() {
        let frame = array.child_value(i);
        let bytes = frame.data();
        anyhow::ensure!(
            bytes.len() == FRAME_SIZE,
            "the stored print holds a frame of {} instead of {FRAME_SIZE} bytes",
            bytes.len()
        );
        frames.push(bytes.to_vec());
    }

    anyhow::ensure!(!frames.is_empty(), "the stored print holds no frame");

    Ok(frames)
}
