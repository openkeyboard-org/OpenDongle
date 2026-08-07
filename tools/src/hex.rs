//! Intel HEX loader and firmware loading. Ports `parse_intel_hex` and
//! `load_firmware` from `flash_dongle.py`.

use std::fs;
use std::path::Path;

use anyhow::{anyhow, bail, Result};

/// Slot A's base, uniform across the supported chips. Still the address a
/// factory image places the application at, and the one a `.bin` with no
/// embedded base is assumed to be linked for.
const APP_BASE: u32 = 0x2000;

/// The addresses OpenBoot may hand control to, PER CHIP FAMILY. Slot A is
/// uniform; slot B is chip-specific, so the pair is only meaningful once the
/// ODG2 family byte says which chip the image is for.
///
/// Checking a family-independent union would accept a CH570 image based at
/// 0x39000 — CH592's slot B, and an address that on CH570 sits inside the RF
/// bond page's neighbourhood rather than a slot. OpenBoot would refuse it later
/// (base != write_base), but late rejection is worse than early: the point of a
/// host-side check is to fail before anyone gets as far as a device.
fn slot_bases_for_family(family: u8) -> Option<[u32; 2]> {
    match family {
        0x70 => Some([0x2000, 0x1E000]), // CH570: OB_APP_END clamped to 0x3A000
        0x92 => Some([0x2000, 0x39000]), // CH592: app region to 0x70000
        _ => None,
    }
}
const MIN_APP_LEN: usize = 0x1000;
const ODG2_OFFSET: usize = 0x20;
const ODG2_LEN: usize = 0x20;
const ODG2_CRC_OFFSET: usize = ODG2_OFFSET + 0x10;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Odg2Identity {
    pub family: u8,
    pub image_len: u32,
    pub image_crc32: u32,
    pub build_id: u32,
}

#[derive(Debug, PartialEq, Eq)]
pub struct FirmwareImage {
    pub base: u32,
    pub bytes: Vec<u8>,
    pub odg2: Option<Odg2Identity>,
}

impl FirmwareImage {
    /// CH570/CH592 implement the ODG2/OSW2 boot chain. CH582 remains a
    /// supported legacy IAP target until its firmware adopts that contract.
    pub fn validate_for_device(&self, device_family: u8) -> Result<()> {
        match self.odg2 {
            Some(identity) if identity.family != device_family => bail!(
                "ODG2 image is for family 0x{:02X}, but the connected device is 0x{device_family:02X}",
                identity.family
            ),
            Some(_) => Ok(()),
            None if matches!(device_family, 0x70 | 0x92) => bail!(
                "the connected CH570/CH592 requires an ODG2 application image"
            ),
            None => Ok(()),
        }
    }
}

fn le32(bytes: &[u8]) -> u32 {
    u32::from_le_bytes(bytes.try_into().unwrap())
}

fn parse_odg2(base: u32, image: &[u8]) -> Result<Option<Odg2Identity>> {
    if image.len() < ODG2_OFFSET + ODG2_LEN || &image[ODG2_OFFSET..ODG2_OFFSET + 4] != b"ODG2" {
        return Ok(None);
    }
    let h = &image[ODG2_OFFSET..ODG2_OFFSET + ODG2_LEN];
    if h[4] != 2 {
        bail!("unsupported ODG2 format {}", h[4]);
    }
    if h[6] != 1 {
        bail!("ODG2 image kind {} is not an application", h[6]);
    }
    if h[7] as usize != ODG2_LEN {
        bail!("ODG2 header length {} is not {ODG2_LEN}", h[7]);
    }
    let header_base = le32(&h[8..12]);
    let image_len = le32(&h[12..16]);
    let image_crc32 = le32(&h[16..20]);
    let build_id = le32(&h[20..24]);
    let flags = le32(&h[24..28]);
    let extension_len = le32(&h[28..32]);
    // Under OpenBoot's A/B slots an application is linked once per slot, so a
    // valid image may be based at slot B (0x1E000 on CH570, 0x39000 on CH592)
    // rather than slot A. These are two separate questions, and comparing both
    // values against one constant collapsed them: the base must be one OpenBoot
    // will actually jump to, AND the header must agree with where the file says
    // it is loaded. A header claiming slot A on an image linked for slot B is
    // legal by the first test and caught only by the second.
    let family = h[5];
    match slot_bases_for_family(family) {
        Some(bases) if bases.contains(&base) => {}
        Some(bases) => bail!(
            "ODG2 application base 0x{base:X} is not a slot base for chip family \
             0x{family:02X} (expected 0x{:X} or 0x{:X})",
            bases[0],
            bases[1]
        ),
        // An unsupported family cannot simply skip the base check. ODG2 is
        // only produced for CH570/CH592; the legacy CH582 path carries no
        // ODG2 header at all, so a header claiming any other family is
        // malformed by construction. Skipping left such an image free to
        // declare ANY load base and still pass validate_for_device, which
        // only compares the family against the connected device.
        None => bail!(
            "ODG2 header declares chip family 0x{family:02X}, which has no slot \
             layout (ODG2 is only produced for CH570 and CH592)"
        ),
    }
    if base != header_base {
        bail!(
            "ODG2 header base 0x{header_base:X} does not match the image's load base 0x{base:X}"
        );
    }
    if image.len() < MIN_APP_LEN {
        bail!(
            "ODG2 image is only {} bytes; minimum is {MIN_APP_LEN}",
            image.len()
        );
    }
    if !image.len().is_multiple_of(4) {
        bail!("ODG2 image length {} is not 4-byte aligned", image.len());
    }
    if image_len as usize != image.len() {
        bail!(
            "ODG2 image length {image_len} does not match file size {}",
            image.len()
        );
    }
    if flags != 0 || extension_len != 0 {
        bail!(
            "ODG2 format 2 requires zero flags/extensions (flags=0x{flags:08X}, extension_len={extension_len})"
        );
    }

    let mut hasher = crc32fast::Hasher::new();
    hasher.update(&image[..ODG2_CRC_OFFSET]);
    hasher.update(&[0u8; 4]);
    hasher.update(&image[ODG2_CRC_OFFSET + 4..]);
    let actual_crc = hasher.finalize();
    if image_crc32 != actual_crc {
        bail!(
            "ODG2 whole-image CRC mismatch: header=0x{image_crc32:08X}, actual=0x{actual_crc:08X}"
        );
    }
    Ok(Some(Odg2Identity {
        family: h[5],
        image_len,
        image_crc32,
        build_id,
    }))
}

fn u32_from_hex(s: &str) -> Result<u32> {
    u32::from_str_radix(s, 16).map_err(|e| anyhow!("invalid hex '{s}': {e}"))
}

fn hex_to_bytes(s: &str) -> Result<Vec<u8>> {
    if !s.len().is_multiple_of(2) {
        bail!("odd-length hex field: '{s}'");
    }
    (0..s.len())
        .step_by(2)
        .map(|i| {
            u8::from_str_radix(&s[i..i + 2], 16)
                .map_err(|e| anyhow!("invalid hex byte '{}': {e}", &s[i..i + 2]))
        })
        .collect()
}

/// Parse Intel HEX text into `(base_addr, image)`. Gaps between records are
/// filled with 0xFF; type 2/4 records set the segment/linear base. Returns
/// `(0, [])` for input with no data records.
pub fn parse_intel_hex(text: &str) -> Result<(u32, Vec<u8>)> {
    let mut base: u32 = 0;
    let mut records: Vec<(u32, Vec<u8>)> = Vec::new();
    let mut min_a: Option<u32> = None;
    let mut max_a: u32 = 0;

    let normalized = text.replace("\r\n", "\n");
    for line in normalized.split('\n') {
        let line = line.trim();
        if !line.starts_with(':') {
            continue;
        }
        if !line.is_ascii() {
            bail!("intel hex: non-ASCII record: {line}");
        }
        if line.len() < 11 {
            bail!("intel hex: record too short: {line}");
        }
        let n = u32_from_hex(&line[1..3])?;
        let addr = u32_from_hex(&line[3..7])?;
        let t = u32_from_hex(&line[7..9])?;
        let data_end = 9 + (n as usize) * 2;
        if line.len() < data_end {
            bail!("intel hex: truncated record: {line}");
        }
        // Verify the record checksum: the trailing byte is the two's complement
        // of everything before it, so the whole record sums to zero mod 256.
        //
        // This was never read at all, so a corrupted record was accepted and
        // programmed. The ODG2 whole-image CRC catches most such damage
        // downstream, but not on the CH582 legacy path, which carries no ODG2
        // header - and checking here costs nothing and localises the fault to
        // the record rather than the image.
        if line.len() < data_end + 2 {
            bail!("intel hex: record is missing its checksum byte: {line}");
        }
        let checked = hex_to_bytes(&line[1..data_end + 2])?;
        let sum = checked.iter().fold(0u8, |acc, b| acc.wrapping_add(*b));
        if sum != 0 {
            bail!(
                "intel hex: bad record checksum (record bytes sum to 0x{sum:02X}, \
                 expected 0x00): {line}"
            );
        }
        let data_hex = &line[9..data_end];
        match t {
            0 => {
                let data = hex_to_bytes(data_hex)?;
                // Checked, because `base` comes from a type-4 record and is
                // attacker-controlled up to 0xFFFF0000: unchecked it panics in
                // a debug build and silently wraps in a release one, which puts
                // max_a below min_a and panics further down instead of
                // reporting a bad file.
                let full = base.checked_add(addr).ok_or_else(|| {
                    anyhow!(
                        "intel hex: record address 0x{base:08X}+0x{addr:04X} \
                         overflows the 32-bit address space: {line}"
                    )
                })?;
                let end = full.checked_add(n).ok_or_else(|| {
                    anyhow!(
                        "intel hex: record at 0x{full:08X} with {n} data byte(s) \
                         runs past the end of the 32-bit address space: {line}"
                    )
                })?;
                records.push((full, data));
                if min_a.is_none_or(|m| full < m) {
                    min_a = Some(full);
                }
                if end > max_a {
                    max_a = end;
                }
            }
            1 => break,
            2 => base = u32_from_hex(data_hex)? << 4,
            4 => base = u32_from_hex(data_hex)? << 16,
            _ => {} // ignore other record types (matches the Python's no-op)
        }
    }

    let Some(min_a) = min_a else {
        return Ok((0, Vec::new()));
    };
    // A file with records at both ends of the address space is legal Intel HEX
    // and would ask for an allocation of nearly 4 GiB to hold a few real bytes.
    // Nothing this tool flashes is remotely that large - the biggest part in
    // the family carries 512 KiB of code flash - so treat a span beyond a
    // generous ceiling as a malformed file rather than trying to serve it.
    const MAX_IMAGE_SPAN: u32 = 4 * 1024 * 1024;
    let span = max_a - min_a;
    if span > MAX_IMAGE_SPAN {
        bail!(
            "intel hex: records span 0x{span:X} bytes (0x{min_a:08X}..0x{max_a:08X}), \
             more than the {} MiB ceiling; this is not a firmware image for this family",
            MAX_IMAGE_SPAN / (1024 * 1024)
        );
    }
    let mut img = vec![0xFFu8; span as usize];
    for (a, d) in records {
        let off = (a - min_a) as usize;
        img[off..off + d.len()].copy_from_slice(&d);
    }
    Ok((min_a, img))
}

/// Load and validate a firmware file. `.hex` is parsed as Intel HEX; anything
/// else is a flat binary assumed based at 0x2000 (the app base under
/// OpenBoot). ODG2 images are integrity-checked here; legacy images are
/// accepted only for the device-family gate to decide. Combined factory
/// images (OpenBoot + app) are always refused.
pub fn load_firmware(path: &Path) -> Result<FirmwareImage> {
    let raw = fs::read(path)?;
    let is_hex = path
        .extension()
        .map(|e| e.eq_ignore_ascii_case("hex"))
        .unwrap_or(false);
    let (base, img) = if is_hex {
        let text = String::from_utf8_lossy(&raw);
        parse_intel_hex(&text)?
    } else {
        // A raw binary carries no base of its own, so slot A is assumed. That
        // assumption is checked immediately: parse_odg2 below requires the
        // ODG2 header to agree, so a slot-B image handed over as a bare .bin
        // is rejected rather than silently treated as slot A. Use an .obb
        // bundle (or Intel HEX) when the slot matters.
        (APP_BASE, raw)
    };
    // A factory image is OpenBoot at 0 with the app at 0x2000, so its ODG2
    // magic sits at 0x2020 instead of 0x20.
    if img.len() >= 0x2024 && &img[0x2020..0x2024] == b"ODG2" && &img[0x20..0x24] != b"ODG2" {
        bail!(
            "{} is a FACTORY image (OpenBoot + app). Updates take the app \
             image only (opendongle-<chip>-product.bin); factory images are \
             flashed at 0x0 with the debug probe.",
            path.display()
        );
    }
    let odg2 = parse_odg2(base, &img)?;
    Ok(FirmwareImage {
        base,
        bytes: img,
        odg2,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A type-4 record can push `base` to 0xFFFF0000; the address arithmetic
    /// below it must report a bad file rather than wrapping (release) or
    /// panicking (debug).
    #[test]
    fn type4_base_near_the_top_cannot_overflow_the_address() {
        // base = 0xFFFF0000, then 16 data bytes at 0xFFF0: the record starts
        // at 0xFFFFFFF0 and runs one byte past the top of the space.
        let hex = ":02000004FFFFFC\n:10FFF000000102030405060708090A0B0C0D0E0F89\n:00000001FF\n";
        let err = parse_intel_hex(hex).unwrap_err().to_string();
        assert!(
            err.contains("32-bit address space"),
            "expected an address-space error, got: {err}"
        );
    }

    /// Records at both ends of the address space are legal Intel HEX and would
    /// otherwise ask for a ~4 GiB allocation to hold a handful of real bytes.
    #[test]
    fn an_absurd_span_is_rejected_rather_than_allocated() {
        // One byte at 0x00000000 and one at 0x7FFF0000: a ~2 GiB span.
        let hex = ":0100000000FF\n:020000047FFF7C\n:0100000000FF\n:00000001FF\n";
        match parse_intel_hex(hex) {
            Err(e) => assert!(
                e.to_string().contains("ceiling"),
                "expected the span ceiling to fire, got: {e}"
            ),
            Ok((base, img)) => panic!(
                "a 2 GiB span was accepted: base=0x{base:08X} len={}",
                img.len()
            ),
        }
    }

    fn odg2_image(family: u8) -> Vec<u8> {
        odg2_image_at(family, APP_BASE)
    }

    fn odg2_image_at(family: u8, base: u32) -> Vec<u8> {
        let mut image = vec![0xA5; MIN_APP_LEN];
        let image_len = image.len() as u32;
        let h = &mut image[ODG2_OFFSET..ODG2_OFFSET + ODG2_LEN];
        h.fill(0);
        h[..4].copy_from_slice(b"ODG2");
        h[4] = 2;
        h[5] = family;
        h[6] = 1;
        h[7] = ODG2_LEN as u8;
        h[8..12].copy_from_slice(&base.to_le_bytes());
        h[12..16].copy_from_slice(&image_len.to_le_bytes());
        h[20..24].copy_from_slice(&0x1234_5678u32.to_le_bytes());
        let mut hasher = crc32fast::Hasher::new();
        hasher.update(&image);
        let crc = hasher.finalize();
        image[ODG2_CRC_OFFSET..ODG2_CRC_OFFSET + 4].copy_from_slice(&crc.to_le_bytes());
        image
    }

    fn to_hex(b: &[u8]) -> String {
        b.iter().map(|x| format!("{x:02x}")).collect()
    }

    #[test]
    fn parse_with_gap_fill() {
        // Golden from flash_dongle.py parse_intel_hex (see scratchpad vectors).
        let text = ":04000000DEADBEEFC4\r\n:020010001122BB\r\n:00000001FF\r\n";
        let (base, img) = parse_intel_hex(text).unwrap();
        assert_eq!(base, 0x0000);
        assert_eq!(to_hex(&img), "deadbeefffffffffffffffffffffffff1122");
        assert_eq!(img.len(), 18);
    }

    #[test]
    fn parse_extended_linear_address() {
        // type-4 record sets linear base 0x0001<<16; data record at 0x10000.
        let golden = ":020000040001F9\n:02000000ABCD86\n:00000001FF\n";
        let (base, img) = parse_intel_hex(golden).unwrap();
        assert_eq!(base, 0x00010000);
        assert_eq!(to_hex(&img), "abcd");
    }

    #[test]
    fn empty_returns_zero() {
        let (base, img) = parse_intel_hex(":00000001FF\n").unwrap();
        assert_eq!(base, 0);
        assert!(img.is_empty());
    }

    /// The record checksum was never read, so a corrupted record parsed
    /// cleanly and was programmed. Downstream the ODG2 whole-image CRC catches
    /// most of it, but the CH582 legacy path has no ODG2 header at all.
    #[test]
    fn corrupt_record_checksum_is_rejected() {
        // Same record as the golden above with one data nibble flipped, so the
        // stored checksum no longer matches: ABCD -> ABCE.
        let err = parse_intel_hex(":02000000ABCE86\n:00000001FF\n").unwrap_err();
        assert!(
            err.to_string().contains("bad record checksum"),
            "expected a checksum rejection, got: {err}"
        );
    }

    #[test]
    fn corrupt_checksum_byte_itself_is_rejected() {
        let err = parse_intel_hex(":02000000ABCD87\n:00000001FF\n").unwrap_err();
        assert!(err.to_string().contains("bad record checksum"), "got: {err}");
    }

    #[test]
    fn record_missing_its_checksum_is_rejected() {
        let err = parse_intel_hex(":02000000ABCD\n").unwrap_err();
        assert!(
            err.to_string().contains("missing its checksum"),
            "got: {err}"
        );
    }

    /// Guards the guard: the valid records used elsewhere must still parse, or
    /// the rejections above would just mean the check rejects everything.
    #[test]
    fn valid_records_still_accepted() {
        let (base, img) = parse_intel_hex(":02000000ABCD86\n:00000001FF\n").unwrap();
        assert_eq!(base, 0);
        assert_eq!(to_hex(&img), "abcd");
    }

    #[test]
    fn factory_image_rejected() {
        // OpenBoot-era factory: bootloader bytes at 0, ODG2 app at 0x2000 —
        // so the ODG2 magic sits at 0x2020 and NOT at 0x20.
        let mut buf = vec![0u8; 0x3000];
        buf[0x2020..0x2024].copy_from_slice(b"ODG2");
        let dir = std::env::temp_dir();
        let p = dir.join("flash_dongle_rs_factory_test.bin");
        fs::write(&p, &buf).unwrap();
        let err = load_firmware(&p).unwrap_err();
        assert!(err.to_string().contains("FACTORY image"));
        let _ = fs::remove_file(&p);
    }

    #[test]
    fn raw_bin_base_0x2000() {
        let dir = std::env::temp_dir();
        let p = dir.join("flash_dongle_rs_raw_test.bin");
        fs::write(&p, [0xAAu8, 0xBB, 0xCC]).unwrap();
        let image = load_firmware(&p).unwrap();
        assert_eq!(image.base, 0x2000);
        assert_eq!(image.bytes, vec![0xAA, 0xBB, 0xCC]);
        assert_eq!(image.odg2, None);
        let _ = fs::remove_file(&p);
    }

    #[test]
    fn odg2_validates_and_binds_device_family() {
        let image = odg2_image(0x70);
        let identity = parse_odg2(APP_BASE, &image).unwrap().unwrap();
        assert_eq!(identity.family, 0x70);
        assert_eq!(identity.build_id, 0x1234_5678);
        let loaded = FirmwareImage {
            base: APP_BASE,
            bytes: image,
            odg2: Some(identity),
        };
        loaded.validate_for_device(0x70).unwrap();
        assert!(loaded.validate_for_device(0x92).is_err());
    }

    #[test]
    fn odg2_corruption_is_rejected() {
        let mut image = odg2_image(0x92);
        image[0x100] ^= 1;
        assert!(parse_odg2(APP_BASE, &image)
            .unwrap_err()
            .to_string()
            .contains("CRC mismatch"));
    }

    #[test]
    fn odg2_wrong_load_base_is_rejected() {
        let image = odg2_image(0x70);
        assert!(parse_odg2(0, &image)
            .unwrap_err()
            .to_string()
            .contains("not a slot base for chip family"));
    }

    /// Slot B is a legitimate link base under A/B, so an image based there must
    /// parse. Before dual-slot builds this was rejected outright.
    #[test]
    fn odg2_slot_b_bases_are_accepted() {
        for (family, base) in [(0x70u8, 0x1E000u32), (0x92u8, 0x39000u32)] {
            let image = odg2_image_at(family, base);
            let identity = parse_odg2(base, &image)
                .unwrap_or_else(|e| panic!("family 0x{family:02X} base 0x{base:X}: {e}"))
                .expect("identity");
            assert_eq!(identity.family, family);
        }
    }

    /// An ODG2 header for a family with no slot layout is malformed - ODG2 is
    /// only produced for CH570/CH592, and the legacy CH582 path carries no
    /// header at all. Skipping the base check for it let such an image declare
    /// any load base and still pass the family comparison downstream.
    #[test]
    fn odg2_unsupported_family_is_rejected() {
        let image = odg2_image_at(0x82, 0x10000);
        let err = parse_odg2(0x10000, &image).unwrap_err().to_string();
        assert!(err.contains("no slot layout"), "got: {err}");
        // Even at a legitimate slot-A base, the family alone disqualifies it.
        let image = odg2_image_at(0x82, APP_BASE);
        let err = parse_odg2(APP_BASE, &image).unwrap_err().to_string();
        assert!(err.contains("no slot layout"), "got: {err}");
    }

    /// The other chip's slot B is NOT a valid base for this family. A flat
    /// union of every slot address would accept this and leave the rejection
    /// to the device, long after the point where it is cheap.
    #[test]
    fn odg2_other_chips_slot_b_is_rejected() {
        for (family, wrong) in [(0x70u8, 0x39000u32), (0x92u8, 0x1E000u32)] {
            let image = odg2_image_at(family, wrong);
            let err = parse_odg2(wrong, &image).unwrap_err().to_string();
            assert!(
                err.contains("not a slot base for chip family"),
                "family 0x{family:02X} base 0x{wrong:X} got: {err}"
            );
        }
    }

    /// An address that is neither slot base must still be refused. A garbled
    /// geometry read would otherwise produce an image that links and parses
    /// cleanly and then never boots, because the bootloader only ever jumps to
    /// a slot base.
    #[test]
    fn odg2_between_slot_bases_is_rejected() {
        let image = odg2_image_at(0x70, 0x10000);
        assert!(parse_odg2(0x10000, &image)
            .unwrap_err()
            .to_string()
            .contains("not a slot base for chip family"));
    }

    /// The case a single-constant check could not express: both values are
    /// individually legal slot bases, but they disagree with each other. That
    /// is a slot-B image wearing a slot-A header.
    #[test]
    fn odg2_header_claiming_another_slot_is_rejected() {
        let image = odg2_image_at(0x70, APP_BASE); // header says slot A
        let err = parse_odg2(0x1E000, &image).unwrap_err().to_string();
        assert!(err.contains("does not match"), "got: {err}");
    }

    #[test]
    fn modern_devices_reject_legacy_images() {
        let loaded = FirmwareImage {
            base: APP_BASE,
            bytes: vec![0xA5; MIN_APP_LEN],
            odg2: None,
        };
        assert!(loaded.validate_for_device(0x70).is_err());
        assert!(loaded.validate_for_device(0x92).is_err());
        loaded.validate_for_device(0x82).unwrap();
    }
}
