// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! Link-encryption readouts and the entropy seed: commands 0x98 (counters),
//! 0x9A (key-exchange state), 0x9B (library heap, measurement builds), 0x99
//! (X25519 timing, bench builds) and 0x9C (seed). All unarmed.
//!
//! The diagnostics are kept as the validated replies plus a renderer, not as
//! named fields: they are bench instruments whose layout still moves with the
//! firmware, and the rendering is the one place that layout is written down.

use anyhow::{anyhow, Result};

use crate::iap::{
    hexsp, op_crypt_diag, op_kex_bench, op_kex_seed, packet_simple, IapDevice, CMD_CRYPT_DIAG,
    CMD_KEX_SEED, KEX_SEED_BYTES,
};

/* --- entropy seed ------------------------------------------------------- */

/// 64 hex characters -> 32 bytes. The firmware refuses a seed whose bytes are
/// all the same; refuse it here too, with a better message.
pub fn parse_kex_seed(text: &str) -> Result<[u8; KEX_SEED_BYTES]> {
    let text = text.trim();
    if text.len() != 2 * KEX_SEED_BYTES || !text.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(anyhow!(
            "the seed must be exactly 64 hex characters (32 bytes)"
        ));
    }
    let mut seed = [0u8; KEX_SEED_BYTES];
    for (i, byte) in seed.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&text[2 * i..2 * i + 2], 16).unwrap();
    }
    if seed.iter().all(|&b| b == seed[0]) {
        return Err(anyhow!(
            "a seed whose bytes are all the same is not entropy; refused"
        ));
    }
    Ok(seed)
}

/// 32 bytes from this computer's random number generator.
pub fn random_seed() -> Result<[u8; KEX_SEED_BYTES]> {
    let mut seed = [0u8; KEX_SEED_BYTES];
    getrandom::fill(&mut seed)
        .map_err(|e| anyhow!("this computer's random number generator failed: {e}"))?;
    Ok(seed)
}

/// Give the dongle 32 bytes of entropy for its next link-key exchange. The
/// dongle mixes it into its own pool, never uses it in place of that, and
/// loses it when unplugged.
pub fn send_kex_seed(dev: &IapDevice, seed: &[u8; KEX_SEED_BYTES]) -> Result<()> {
    let r = op_kex_seed(dev, seed)?
        .ok_or_else(|| anyhow!("KexSeed: no response (firmware without the key exchange?)"))?;
    if r.len() < 3 || r[0] != CMD_KEX_SEED || r[1] < 1 {
        return Err(anyhow!("KexSeed: unexpected reply; raw={}", hexsp(&r, 8)));
    }
    match r[2] {
        0 => Ok(()),
        1 => Err(anyhow!("KexSeed: the dongle refused the seed's length")),
        2 => Err(anyhow!(
            "KexSeed: the dongle refused a seed whose bytes are all the same"
        )),
        s => Err(anyhow!("KexSeed: status 0x{s:02X}")),
    }
}

/* --- X25519 timing ------------------------------------------------------ */

/// One X25519 scalar multiplication timed on the device, in 60 MHz cycles.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct KexBench {
    pub steps: u32,
    pub total_cycles: u32,
    pub longest_step_cycles: u32,
    pub shortest_step_cycles: u32,
    pub begin_cycles: u32,
    pub finish_cycles: u32,
    /// The first eight bytes of the result, to tell a real run from a stub.
    pub result_prefix: [u8; 8],
}

/// BENCH: time one X25519 scalar multiplication (command 0x99, KEX_BENCH
/// firmware only). The radio is starved while it runs.
pub fn read_kex_bench(dev: &IapDevice) -> Result<KexBench> {
    let r = op_kex_bench(dev)?
        .ok_or_else(|| anyhow!("KexBench: no response (not a KEX_BENCH build?)"))?;
    if r.len() < 34 || r[0] != 0x99 || r[1] != 32 {
        return Err(anyhow!("KexBench: unexpected reply; raw={}", hexsp(&r, 8)));
    }
    let word = |at: usize| u32::from_le_bytes(r[at..at + 4].try_into().unwrap());
    Ok(KexBench {
        total_cycles: word(2),
        longest_step_cycles: word(6),
        shortest_step_cycles: word(10),
        steps: word(14),
        result_prefix: r[18..26].try_into().unwrap(),
        begin_cycles: word(26),
        finish_cycles: word(30),
    })
}

pub fn render_kex_bench(b: &KexBench) -> Vec<String> {
    let us = |cycles: u32| f64::from(cycles) / 60.0;
    vec![
        "x25519 on device (60 MHz, interrupts masked per step):".to_string(),
        format!("  steps           {}", b.steps),
        format!(
            "  total           {} cycles ({:.1} ms)",
            b.total_cycles,
            us(b.total_cycles) / 1000.0
        ),
        format!(
            "  mean step       {:.1} us",
            us(b.total_cycles) / f64::from(b.steps)
        ),
        format!(
            "  longest step    {} cycles ({:.1} us)",
            b.longest_step_cycles,
            us(b.longest_step_cycles)
        ),
        format!(
            "  shortest step   {} cycles ({:.1} us)",
            b.shortest_step_cycles,
            us(b.shortest_step_cycles)
        ),
        format!(
            "  begin / finish  {:.1} us / {:.1} us",
            us(b.begin_cycles),
            us(b.finish_cycles)
        ),
        format!("  result[0..8]    {}", hexsp(&b.result_prefix, 8)),
    ]
}

/* --- link-encryption diagnostics ---------------------------------------- */

/// The link-encryption counters, with the key-exchange and heap readouts
/// where the firmware has them.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CryptDiag {
    /// The 0x98 reply, framing checked.
    counters: Vec<u8>,
    /// The 0x9A reply, on firmware with the key exchange.
    kex: Option<Vec<u8>>,
    /// The 0x9B reply, on measurement builds.
    heap: Option<Vec<u8>>,
}

/// Read the link-encryption counters (command 0x98: frames verified and
/// frames dropped, by reason), then ask for the two optional readouts. A
/// firmware that does not answer those simply has none; that is not an error.
pub fn read_crypt_diag(dev: &IapDevice) -> Result<CryptDiag> {
    let r = op_crypt_diag(dev)?.ok_or_else(|| anyhow!("CryptDiag: no response (timeout)"))?;
    if r.len() < 2 || r[0] != CMD_CRYPT_DIAG || (r[1] as usize) < 28 || r.len() < 2 + r[1] as usize
    {
        return Err(anyhow!("CryptDiag: unexpected reply; raw={}", hexsp(&r, 8)));
    }
    let optional = |cmd: u8, min_payload: u8| {
        dev.xfer(&packet_simple(cmd, &[]), 500)
            .ok()
            .flatten()
            .filter(|k| {
                k.len() >= 2 + usize::from(min_payload) && k[0] == cmd && k[1] >= min_payload
            })
    };
    Ok(CryptDiag {
        counters: r,
        kex: optional(0x9A, 8),
        heap: optional(0x9B, 28),
    })
}

pub fn render_crypt_diag(diag: &CryptDiag) -> Vec<String> {
    let mut out = Vec::new();
    let r = &diag.counters;
    let word = |at: usize| u32::from_le_bytes(r[at..at + 4].try_into().unwrap());
    let n = r[1] as usize;
    out.push(format!(
        "link encryption (payload {n} B{}):",
        if n == 42 { ", product layout" } else { "" }
    ));
    out.push(format!("  verified        {}", word(2)));
    const REASON: [&str; 6] = ["(ok slot)", "shape", "inactive", "mac", "replay", "engine"];
    for (i, name) in REASON.iter().enumerate().skip(1) {
        out.push(format!("  drop {:<11}{}", name, word(6 + 4 * i)));
    }
    if n == 42 {
        out.push(format!("  aes redo        {}", word(30)));
        out.push(format!("  announce retry  {}", word(34)));
        out.push(format!("  boot KAT        run={} fail={}", r[38], r[39]));
        out.push(format!("  plaintext drop  {}", word(40)));
    }

    // Link-key exchange counters (0x9A), where the firmware has them.
    if let Some(k) = &diag.kex {
        const FAIL: [&str; 6] = [
            "none",
            "low-order key",
            "AES engine",
            "confirmation",
            "peer abort",
            "timeout",
        ];
        const STATE: [&str; 7] = [
            "idle",
            "keygen",
            "public keys",
            "shared secret",
            "confirm",
            "done",
            "failed",
        ];
        out.push("key exchange:".to_string());
        out.push(format!("  started         {}", k[2]));
        out.push(format!("  completed       {}", k[3]));
        out.push(format!(
            "  last failure    {}",
            FAIL.get(k[4] as usize).unwrap_or(&"?")
        ));
        out.push(format!(
            "  state           {}",
            STATE.get(k[5] as usize).unwrap_or(&"?")
        ));
        out.push(format!(
            "  entropy samples {}",
            u16::from_le_bytes([k[6], k[7]])
        ));
        out.push(format!("  commit failures {}", k[8]));
        out.push(format!("  tried this link {}", k[9]));
        if k[1] >= 20 && k.len() >= 22 {
            out.push(format!("  failures        {}", k[19]));
            out.push(format!("  dongle x25519   phase {} count {}", k[20], k[21]));
            let p = &k[10..18];
            out.push(format!("keyboard status (frames received: {}):", k[18]));
            out.push(format!(
                "  state           {}",
                STATE.get(p[0] as usize).unwrap_or(&"?")
            ));
            out.push(format!(
                "  last failure    {}",
                FAIL.get(p[1] as usize).unwrap_or(&"?")
            ));
            out.push(format!(
                "  candidate={} keys_ready={} dongle_confirm_ok={} dongle_pub_mask=0x{:X} dongle_conf_mask=0x{:X}",
                p[2] & 1,
                (p[2] >> 1) & 1,
                (p[2] >> 2) & 1,
                p[2] >> 4,
                p[6]
            ));
            out.push(format!(
                "  x25519          phase {} count {} op {}",
                p[3], p[4], p[5]
            ));
            out.push(format!("  sessions ok     {}", p[7]));
            out.push(format!(
                "  seeded by MCU   {}",
                if p[2] & 0x08 != 0 { "yes" } else { "no" }
            ));
        }
        if k[1] >= 22 && k.len() >= 24 {
            out.push("dongle entropy seed:".to_string());
            out.push(format!("  received        {}", k[22]));
            out.push(format!("  used in draws   {}", k[23] & 0x7F));
            out.push(format!(
                "  one waiting     {}",
                if k[23] & 0x80 != 0 { "yes" } else { "no" }
            ));
        }
    }

    // BLE library heap watermark (0x9B), measurement builds only.
    if let Some(h) = &diag.heap {
        let w = |i: usize| u32::from_le_bytes(h[2 + 4 * i..6 + 4 * i].try_into().unwrap());
        out.push("library heap (measurement build):".to_string());
        out.push(format!("  size            {} B", w(0)));
        out.push(format!("  untouched       {} B", w(1)));
        out.push(format!(
            "  highest touched {} B from the start ({} B never reached at the end)",
            w(2),
            w(0).wrapping_sub(w(2))
        ));
        out.push(format!("  largest gap     {} B at offset {}", w(3), w(4)));
        out.push(format!(
            "  after start-up  untouched {} B, highest touched {} B",
            w(5),
            w(6)
        ));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scripted::ScriptedLink;

    #[test]
    fn kex_seed_text_is_64_hex_characters_and_not_one_repeated_byte() {
        let good = "000102030405060708090a0b0c0d0e0f101112131415161718191A1B1C1D1E1F";
        let seed = parse_kex_seed(good).unwrap();
        assert_eq!(seed[0], 0x00);
        assert_eq!(seed[31], 0x1F);
        assert!(parse_kex_seed(&good[..62]).is_err());
        assert!(parse_kex_seed(&format!("{good}00")).is_err());
        assert!(parse_kex_seed(&"zz".repeat(32)).is_err());
        assert!(parse_kex_seed(&"00".repeat(32)).is_err());
        assert!(parse_kex_seed(&"ff".repeat(32)).is_err());
        assert!(parse_kex_seed(&"5a".repeat(32)).is_err());
    }

    #[test]
    fn a_random_seed_is_not_one_repeated_byte() {
        // What the firmware refuses. 32 equal bytes by chance is 2^-248.
        let seed = random_seed().unwrap();
        assert!(seed.iter().any(|&b| b != seed[0]));
    }

    #[test]
    fn a_heap_reading_past_its_size_does_not_panic() {
        // Device-reported: "highest touched" above "size" must not underflow.
        let mut counters = vec![0x98, 28];
        counters.resize(64, 0);
        let mut heap = vec![0x9B, 28];
        heap.extend_from_slice(&100u32.to_le_bytes()); // size
        heap.extend_from_slice(&0u32.to_le_bytes());
        heap.extend_from_slice(&200u32.to_le_bytes()); // highest touched
        heap.resize(64, 0);
        let link = ScriptedLink::new(move |cmd, _| match cmd {
            0x98 => Some(counters.clone()),
            0x9B => Some(heap.clone()),
            _ => None,
        });
        let diag = read_crypt_diag(&link.device()).unwrap();
        assert!(render_crypt_diag(&diag)
            .iter()
            .any(|l| l.contains("highest touched 200 B")));
    }

    #[test]
    fn optional_readouts_are_absent_not_errors() {
        let mut counters = vec![0x98, 28];
        counters.resize(64, 0);
        let link = ScriptedLink::new(move |cmd, _| (cmd == 0x98).then(|| counters.clone()));
        let diag = read_crypt_diag(&link.device()).unwrap();
        let text = render_crypt_diag(&diag).join("\n");
        assert!(
            text.starts_with("link encryption (payload 28 B):"),
            "{text}"
        );
        assert!(!text.contains("key exchange"), "{text}");
        assert!(!text.contains("library heap"), "{text}");
        assert_eq!(link.commands(), vec![0x98, 0x9A, 0x9B]);
    }
}
