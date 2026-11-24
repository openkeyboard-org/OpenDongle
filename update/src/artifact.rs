// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! An update bundle, checked for everything that can be checked with no
//! device attached.
//!
//! The `.obb` container alone is not enough to go on. It carries addresses,
//! lengths and CRCs, and a chip family; it does not say what the code IS. An
//! OpenController keyboard bundle for the CH592 has the same header, the same
//! chip family and the same two slot bases as an OpenDongle CH592 bundle, and
//! OpenBoot — which attests only a length and a CRC — would flash either. What
//! tells them apart is the ODG2 header inside each image, so this reads it.

use std::path::Path;

use anyhow::{bail, Context, Result};

use openboot::bundle::{Bundle, Source, FAMILY_UNSPECIFIED};
use openboot::proto::consts::{OB_FAMILY_CH570, OB_FAMILY_CH592};
use opendongle::hex::{identify_image, slot_bases_for_family};

/// Larger than any bundle can be (two full CH592 slots are under 450 KiB), so
/// a file picked by mistake is refused before it is read into memory.
const MAX_BUNDLE_BYTES: u64 = 1024 * 1024;

/// The dongle families, as each side numbers them: the ODG2 family byte the
/// application reports, and the `OB_FAMILY_*` the bootloader and the bundle
/// header use. They are different numbering spaces.
const FAMILIES: [(u8, u8, &str); 2] = [
    (0x70, OB_FAMILY_CH570, "CH570"),
    (0x92, OB_FAMILY_CH592, "CH592"),
];

/// One per-slot build inside a bundle.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Variant {
    /// The slot base this build was linked for.
    pub base: u32,
    pub len: u32,
    /// Differs between the two builds of one release: the slot base is part
    /// of what is hashed. It is what the running application reports, so it
    /// is how a finished update is recognised.
    pub build_id: u32,
}

/// A bundle that is intact, is an OpenDongle application, and carries the
/// build for both slots of one chip family.
#[derive(Debug, Clone)]
pub struct Artifact {
    source: Source,
    /// The ODG2 family byte: what the dongle's application reports for itself.
    pub family: u8,
    /// Slot A's build, then slot B's.
    pub variants: [Variant; 2],
}

impl Artifact {
    /// Read and check a bundle file.
    pub fn load(path: &Path) -> Result<Artifact> {
        let size = std::fs::metadata(path)
            .with_context(|| format!("read {}", path.display()))?
            .len();
        if size > MAX_BUNDLE_BYTES {
            bail!(
                "{} is {size} bytes; an update bundle is never that large",
                path.display()
            );
        }
        let raw = std::fs::read(path).with_context(|| format!("read {}", path.display()))?;
        Artifact::parse(&raw).with_context(|| format!("{}", path.display()))
    }

    /// Check bundle bytes. Every refusal says which rule was broken.
    pub fn parse(raw: &[u8]) -> Result<Artifact> {
        // Container: magic, version, whole-file CRC, table bounds, per-image
        // CRC, ordering, no overlap.
        let bundle = Bundle::parse(raw)?;

        if bundle.chip_family == FAMILY_UNSPECIFIED {
            bail!(
                "the bundle does not say which chip it is for; an update bundle \
                 must be built with --chip"
            );
        }
        if bundle.images.len() != 2 {
            bail!(
                "the bundle carries {} build(s); an update needs exactly two, one \
                 per slot, because the dongle alternates the slot it writes",
                bundle.images.len()
            );
        }

        // Each image must be an OpenDongle application linked for the base
        // the bundle files it under. This is where a keyboard image, a factory
        // image, or a truncated one is turned away.
        let mut variants = Vec::with_capacity(2);
        let mut family = None;
        for image in &bundle.images {
            let id = identify_image(image.base, &image.bytes)
                .with_context(|| not_an_update(image.base, &image.bytes))?;
            if *family.get_or_insert(id.family) != id.family {
                bail!("the two builds in the bundle are for different chip families");
            }
            variants.push(Variant {
                base: image.base,
                len: id.image_len,
                build_id: id.build_id,
            });
        }
        let family = family.expect("two images were identified");

        // The bundle header and the images must agree about the chip.
        let Some(&(_, chip, name)) = FAMILIES.iter().find(|f| f.0 == family) else {
            bail!("ODG2 family 0x{family:02X} is not a dongle this tool updates");
        };
        if bundle.chip_family != chip {
            bail!(
                "the bundle header says {} but its images are built for {name}",
                bundle.family_name()
            );
        }

        // And between them they must cover both slots. `identify_image` has
        // already held each base to the family's pair, and the container
        // refuses a duplicate base, so two images means both — but say so
        // here, where it is the rule being relied on.
        let slots = slot_bases_for_family(family).expect("identified families have a layout");
        let bases = [variants[0].base, variants[1].base];
        if bases != slots {
            bail!(
                "the bundle's builds are at 0x{:08X} and 0x{:08X}; {name} needs \
                 0x{:08X} and 0x{:08X}",
                bases[0],
                bases[1],
                slots[0],
                slots[1]
            );
        }

        Ok(Artifact {
            source: Source::Bundle(bundle),
            family,
            variants: [variants[0], variants[1]],
        })
    }

    /// What to hand OpenBoot's flash: it picks the build for the slot the
    /// device offers.
    pub fn source(&self) -> &Source {
        &self.source
    }

    /// The build linked for `base`.
    pub fn variant_at(&self, base: u32) -> Option<&Variant> {
        self.variants.iter().find(|v| v.base == base)
    }

    pub fn chip_name(&self) -> &'static str {
        FAMILIES
            .iter()
            .find(|f| f.0 == self.family)
            .map(|f| f.2)
            .expect("a parsed artifact has a known family")
    }
}

/// Context for an image that failed identification, naming the two mistakes
/// that are easy to make.
fn not_an_update(base: u32, image: &[u8]) -> String {
    let hint = if image.len() >= 0x2024 && &image[0x2020..0x2024] == b"ODG2" {
        // A factory image is bootloader + application: its ODG2 header sits
        // 0x2000 further in. Those are written at address 0 with a debug
        // probe, never over USB.
        " (this looks like a FACTORY image, which is programmed with a debug \
         probe, not flashed as an update)"
    } else {
        ""
    };
    format!("the build at 0x{base:08X} is not an OpenDongle update image{hint}")
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use openboot::image::Image;

    /// A minimal, valid application image: one page, with a finalized ODG2
    /// header at 0x20.
    pub(crate) fn app_image(family: u8, base: u32, build_id: u32) -> Vec<u8> {
        let mut image = vec![0xA5u8; 0x1000];
        image[0x20..0x24].copy_from_slice(b"ODG2");
        image[0x24] = 2; // format
        image[0x25] = family;
        image[0x26] = 1; // application
        image[0x27] = 0x20; // header length
        image[0x28..0x2C].copy_from_slice(&base.to_le_bytes());
        image[0x2C..0x30].copy_from_slice(&0x1000u32.to_le_bytes());
        image[0x30..0x34].fill(0); // CRC, computed over the image with this zero
        image[0x34..0x38].copy_from_slice(&build_id.to_le_bytes());
        image[0x38..0x40].fill(0); // flags, extension length
        let crc = crc32fast::hash(&image);
        image[0x30..0x34].copy_from_slice(&crc.to_le_bytes());
        image
    }

    pub(crate) const CH592_BASES: [u32; 2] = [0x2000, 0x39000];
    pub(crate) const CH570_BASES: [u32; 2] = [0x2000, 0x1E000];

    /// A release bundle: `build` for slot A and `build + 1` for slot B.
    pub(crate) fn bundle(chip: u8, family: u8, bases: [u32; 2], build: u32) -> Vec<u8> {
        let images = vec![
            Image {
                base: bases[0],
                bytes: app_image(family, bases[0], build),
            },
            Image {
                base: bases[1],
                bytes: app_image(family, bases[1], build + 1),
            },
        ];
        Bundle::new(chip, images).unwrap().encode()
    }

    pub(crate) fn ch592_bundle(build: u32) -> Vec<u8> {
        bundle(OB_FAMILY_CH592, 0x92, CH592_BASES, build)
    }

    fn refusal(raw: &[u8]) -> String {
        format!("{:#}", Artifact::parse(raw).unwrap_err())
    }

    #[test]
    fn a_release_bundle_is_accepted_and_describes_both_slots() {
        let artifact = Artifact::parse(&ch592_bundle(0x1000)).unwrap();
        assert_eq!(artifact.family, 0x92);
        assert_eq!(artifact.chip_name(), "CH592");
        assert_eq!(
            artifact.variants,
            [
                Variant {
                    base: 0x2000,
                    len: 0x1000,
                    build_id: 0x1000
                },
                Variant {
                    base: 0x39000,
                    len: 0x1000,
                    build_id: 0x1001
                },
            ]
        );
        assert_eq!(artifact.variant_at(0x39000).unwrap().build_id, 0x1001);
        assert!(artifact.variant_at(0x3000).is_none());

        let ch570 = bundle(OB_FAMILY_CH570, 0x70, CH570_BASES, 7);
        assert_eq!(Artifact::parse(&ch570).unwrap().chip_name(), "CH570");
    }

    /// The case the container cannot catch: OpenController's keyboard firmware
    /// is a CH592 bundle with these exact bases. Only the ODG2 family differs.
    #[test]
    fn a_keyboard_bundle_for_the_same_chip_is_refused() {
        let keyboard = bundle(OB_FAMILY_CH592, 0xB2, CH592_BASES, 1);
        let why = refusal(&keyboard);
        assert!(why.contains("not an OpenDongle update image"), "{why}");
    }

    #[test]
    fn a_bundle_whose_header_and_images_disagree_about_the_chip_is_refused() {
        // CH592 images filed under a CH570 header: the device check against
        // the header alone would let this through onto a CH570.
        let mixed = bundle(OB_FAMILY_CH570, 0x92, CH592_BASES, 1);
        let why = refusal(&mixed);
        assert!(why.contains("says CH570"), "{why}");
        assert!(why.contains("built for CH592"), "{why}");
    }

    #[test]
    fn a_bundle_that_does_not_name_its_chip_is_refused() {
        let unnamed = bundle(FAMILY_UNSPECIFIED, 0x92, CH592_BASES, 1);
        assert!(refusal(&unnamed).contains("--chip"));
    }

    #[test]
    fn a_bundle_with_only_one_slot_is_refused() {
        let one = Bundle::new(
            OB_FAMILY_CH592,
            vec![Image {
                base: 0x2000,
                bytes: app_image(0x92, 0x2000, 1),
            }],
        )
        .unwrap()
        .encode();
        assert!(refusal(&one).contains("exactly two"));
    }

    #[test]
    fn an_image_filed_under_a_base_it_was_not_linked_for_is_refused() {
        // Slot A's build, stored twice: once correctly and once as slot B.
        let images = vec![
            Image {
                base: 0x2000,
                bytes: app_image(0x92, 0x2000, 1),
            },
            Image {
                base: 0x39000,
                bytes: app_image(0x92, 0x2000, 2),
            },
        ];
        let raw = Bundle::new(OB_FAMILY_CH592, images).unwrap().encode();
        assert!(refusal(&raw).contains("0x00039000"));
    }

    #[test]
    fn an_image_with_no_identity_is_refused() {
        let images = vec![
            Image {
                base: 0x2000,
                bytes: vec![0xA5; 0x1000],
            },
            Image {
                base: 0x39000,
                bytes: app_image(0x92, 0x39000, 2),
            },
        ];
        let raw = Bundle::new(OB_FAMILY_CH592, images).unwrap().encode();
        assert!(refusal(&raw).contains("not an OpenDongle update image"));
    }

    #[test]
    fn a_factory_image_is_named_as_one() {
        // Bootloader region, then the application with its header 0x2000 in.
        let mut factory = vec![0u8; 0x2000];
        factory.extend_from_slice(&app_image(0x92, 0x2000, 1));
        let images = vec![
            Image {
                base: 0x2000,
                bytes: factory,
            },
            Image {
                base: 0x39000,
                bytes: app_image(0x92, 0x39000, 2),
            },
        ];
        let raw = Bundle::new(OB_FAMILY_CH592, images).unwrap().encode();
        assert!(refusal(&raw).contains("FACTORY image"));
    }

    #[test]
    fn a_damaged_bundle_is_refused_before_anything_in_it_is_believed() {
        let mut raw = ch592_bundle(1);
        let last = raw.len() - 1;
        raw[last] ^= 0x01;
        assert!(Artifact::parse(&raw).is_err());
        assert!(Artifact::parse(&raw[..raw.len() - 4]).is_err());
        assert!(Artifact::parse(b"not a bundle at all").is_err());
    }
}
