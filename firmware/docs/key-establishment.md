# OpenKeyboard Link Key Establishment — KEXv1

**Status:** DESIGN DRAFT (synthesized 2026-08-17 from three independent design passes:
security-first / implementation-first / migration-first), then revised the same day
against two adversarial reviews (protocol + implementation-fit). Constraints below
marked "verified" were read from code this session; open measurements are marked
[CAMPAIGN DATA PENDING] or [MEASUREMENT REQUIRED]. **Not yet approved for
implementation** — §0 lists the review findings folded in and the two that still
need a decision.

## 0. Adversarial-review corrections (2026-08-17)

Two reviewers (one protocol, one implementation-fit, both with the trees open)
attacked the synthesized draft. The convergent, code-confirmed findings are
already folded into the sections below; recorded here so the change history is
legible and nothing silently reverts.

- **[FIXED, was HIGH] Per-session counter restart re-opened keystream reuse.**
  The draft restarted the keyboard TX counter at 1 per adopted session. Both
  reviewers cited `kbd_rf_crypt.h:43-60`, which documents monotonic-per-key as
  load-bearing *because a `sid` can recur* (a re-served still-live session, §10;
  the §9.3 journal-loss reseed). §9.2 now keeps the counter monotonic for the
  life of the key; sid monotonicity is defence-in-depth, not the sole guarantee.
  The sealed-LED `dctr` is fixed the same way.
- **[FIXED, was MED-HIGH] type_tag bit-7 "capability hint" is not free space.**
  The full byte feeds `% 3`/`% 5` hop seeds and the keyboard's LEN-15 accept
  gate is `rxBuf[6] < 5`, so a `0x82` type_tag desyncs hopping and is dropped
  outright by a v2 keyboard — a one-bit active-DoS, and unreconstructable after
  a dongle reboot (no bond field). The hint is CUT (§4); the DG PUB is the only
  KX signal, plus an optional `& 0x7F` masking hardening.
- **[FIXED, was BLOCKER] CH570 epoch journal mapping.** `BOND_EEPROM_OFF+0x100`
  is inside the bond's own 4 KB erase unit, which `dongle_nv_erase` wipes whole
  — every `bond_save` would destroy the journal. §9.3 relocates it to the
  spare 0x3B000 page (with the seam changes that needs) and makes it a CH570
  enablement prerequisite, not an additive L1 step.
- **[FIXED, was BLOCKER] CH570 L1 is not additive.** The encrypted CH570 image
  is already ~128 B over the 0x800 floor; L1-on-CH570 depends on the stack-floor
  campaign (§14), split out as stage L1b (§16).
- **[FIXED, was HIGH] "zero grid impact" is CH570-only.** On CH592 the poll TX
  runs in TMOS task context, so a KX slice adds bounded latency; §5/§18 budgets
  now say "bounded jitter <= slice, [MEASUREMENT REQUIRED]" for CH592 and reserve
  "zero by construction" for CH570's ISR poll. A "never slice while a
  poll/response event is pending" scheduling rule is required (§19).
- **[OPEN — decision needed] Power-loss commit window (protocol F3).** Dongle
  deferred `bond_save` + keyboard erase-`LK_old`-on-first-announce can strand
  the two ends on different keys with no common fallback => forced re-pair,
  violating §5.4. Proposed: dongle persist-before-announce AND keyboard retains
  `LK_old` in its KBD3 `prev_link_key` slot until a *full reconnect* verifies
  under `LK_new`. Fold into §5.4/§12 once accepted.
- **[OPEN — decision needed] Silent permanent plaintext downgrade (protocol
  F4).** With `KBD_REQUIRE_ENC` OFF (default), an attacker who stays and
  corrupts one KX fragment per attempt holds the pair in a working-but-plaintext
  state invisibly. Proposed: after N failed KX connects, a keyboard-local LED
  warning so "encryption never engaged" is user-visible even when not requiring
  it. Fold into §6/§8 once accepted.
- **[NOTED as invariants] SESSION_REQ needs a new dongle uplink control-frame
  classifier + empty-body MAC-verify verb (impl F6 — larger than "~40 B"; §19
  open item); KDF-vs-direct-CMAC domain separation must be stated normatively
  and asserted in the oracle (protocol F6); EV10 provisional-apply must snapshot
  and restore session+counter verbatim (protocol F5); SESSION_REQ reboot replay
  is an accepted bounded nuisance (protocol F7).**

The reviewers judged the asymmetric core sound: X25519 TOFU + transcript-bound
CMAC confirmation + challenge-fresh announces, reflection/role separation, and
pubkey double-binding all verified with no attack. The exploitable gaps were at
the migration/placement edges — now corrected or flagged above.

**Scope:** OpenController (keyboard TX, CH592F) <-> OpenDongle (receiver, CH592 and
CH570), 2.4 GHz proprietary link, AES-128-CCM data plane. This document is the
production key-establishment lifecycle: on-air ECDH, transcript binding, key
confirmation, per-session keys, announce freshness, downlink protection, rekey,
migration, and the disposition of the bench provisioning path.

**One-sentence summary:** ephemeral-ephemeral X25519 run on the CONNECTED 875 us
poll grid immediately after burst-promote, with every symmetric derivation and MAC
built from AES-128-CMAC (no SHA-256), TOFU at user-initiated physical pairing
upgraded by old-key continuity on every rekey, per-session CCM keys derived from
the link key and a monotonic `epoch||seq` session id, challenge-fresh announces,
a sealed LED downlink, polls explicitly out of scope, and a migration plan in
which the dongle bond record does not change size and every upgrade order of the
two repos yields a working (at worst plaintext) link.

---

## 1. Verified code constraints this design is built on

| Constraint | Where verified |
|---|---|
| Keyboard MCU is CH592F (60 MHz QingKe V4F, 448 KB flash, 26 KB SRAM), same shared HW-AES/BLEB stale-abort hazard as the CH592 dongle | `OpenController/firmware/Makefile:5` |
| CCM wire: enc HID LEN {16,19,22} tags {0xA3,0xA8,0xA1}; plaintext LEN set {1,3,4,7,10,15}; nonce = `sid(4 LE)||dir(1)||ctr(4 LE)||0^4`; dir 0x01 up / 0x02 down reserved; announce (0xA5, LEN 14); cap advert (0xA6, LEN 3) | `common/include/rf_crypt.h:37-65` |
| Dongle CH570 TX DMA is 17 B => **max 15 B TX payload**; RX DMA accepts **32 B** payload | `ch570/src/hal_rf_ch570.c:32,46,126` |
| Keyboard TX buffer 22 B (`KBD_CRYPT_MAX_FRAME`), RX cap 70 B (`RF_RX_MAX_LEN`) | `OpenController/src/rf_task.c:86,218` |
| AES costs (measured on silicon): CH592 HW 871 cyc/block, 838 key schedule; CH570 ASM_A (production default) 1,672 / 7,647 with 116 B stack spare, ASM_F 3,960 / 7,405 with 524 B spare; poll slot = 87,500 cyc @100 MHz; hal_aes is forward-only, NOT reentrant, treat as not constant-time | `common/include/hal_aes.h:49-129`, `ch570/src/hal_aes_ch570_impl.h` |
| CH592 engine stale-abort => `RF_CRYPT_AES_DOUBLE` double-compute is the shipped mitigation; mstatus masking hung both ends and was reverted (e89cd44); announce-seal retry budget 3 | `rf_crypt.h:164-193`, `rf_task.c:1450-1460` |
| Announce: pre-built in task context, transmitted by `rf_send_poll` — the TMR ISR on CH570 — via publish-after-build volatile count; 8 poll slots | `rf_task.c:1439-1448` |
| Crypt RX FIFO 2-deep (CH570 stack floor); silence guard 64 frames ~56 ms; boot pair window 10 x 300 ms | `rf_task.c:1422-1476,552` |
| Dongle bond v2 = 48 B, `BOND_RECORD_MAX_NV 48` static-asserted against both platform scratch buffers; `reserved0` u16 spare, **currently forced 0 on every save**; flags has 6 free bits; logical 0x5000 -> CH592 DataFlash 0x75000 / CH570 reserved code-flash page; v1->v2 migrates in place | `common/include/bond.h:31-73`, `common/src/bond.c:105` |
| Keyboard bond "KBD2" v2 = 40 B in a 256 B DataFlash page at 0x74000; **no v1 migration by design** ("version bump ... re-pairs"); `reserved1` u16 spare | `OpenController/src/rf_task.c:98-118,188` |
| Cap advert version checked `== 1` **exactly** on the dongle => a v2 advert is silently ignored by shipped dongles (safe out-of-order upgrade) | `rf_task.c:1813` |
| Anonymous-advert mislatch residue documented in-tree; "the establishment handshake ... is where it gets closed properly" | `rf_task.c:1400-1420` |
| Pair-ACK `type_tag` (byte [4]) feeds `% 3` / `% 5` hop seeds over the **full byte**, and the keyboard's LEN-15 accept gate is `rxBuf[6] < 5` — high bits are NOT free (see §4, hint cut after review) | `rf_protocol.h:293,298`, `../OpenController/firmware/src/rf_task.c:879` |
| Session mint runs in task context; sid = two draws of xorshift32(UID^RTC^SRAM-PUF) — documented 32-bit birthday residual, "quite inadequate for, say, a key" | `rf_task.c:3298-3341`, `kbd_rf_crypt.h:146-158` |
| Keyboard TX counter monotonic per KEY with per-boot random start (`CTR_START_MAX 2^31`) — the workaround for non-fresh session ids | `kbd_rf_crypt.h:43-60,140-167` |
| IAP: `CMD_BOND_WRITE 0x87` (statuses 0xB2 semantic / 0xB3 key-flag form / 0xB4 persist-verify / 0xB5 deferred apply, live-activate), `CMD_BOND_READ 0x88` **redacts the key** and a redacted view cannot be written back, `CMD_CRYPT_DIAG 0x94`, `CMD_STACK_WATERMARK 0x96` | `common/src/iap.c:34-43,240-295,343-356` |
| Keyboard key-write is UART 0xAE, compiled only under `KBD_CRYPT_BENCH_KEY`; release images have no key-write path | `OpenController/src/keyboard_uart.c:131,215-291` |
| RAM headroom: CH570 is **~128 B OVER** the production 0x800 `CH570_STACK_FLOOR` with the encrypted image today (the product map was linked only under the bench `--defsym` 0x700 override — `bench/STACK-WATERMARK.md`); the earlier "~176 B slack" figure was wrong. CH592 dongle ~6.3 KB free; keyboard ~8.6 KB free. Re-verify at each landing stage with `firmware-size` / `CMD_STACK_WATERMARK`. | product maps, `bench/STACK-WATERMARK.md` |
| No hash and no ECC primitive exists in either tree; AES-128 via `hal_aes` is the only crypto seam | tree-wide |

---

## 2. Threat model (governs every decision; required point 4)

Pairing is user-initiated, at physical proximity, inside a ~3 s dongle replug boot
window (10 x 300 ms) against a ~5.3 s keyboard pair window.

| Adversary | Capability | Outcome |
|---|---|---|
| **A1 — passive sniffer, always present** (including at pairing) | records all RF forever | **Defeated.** X25519 ECDH: no key material ever crosses the air in recoverable form. This is the property no AES-only scheme can provide and the reason an asymmetric primitive is non-negotiable. |
| **A2 — active attacker, post-pairing** | inject / modify / replay, forge announces and EV10 | **Defeated.** CCM on HID with per-session keys, challenge-fresh authenticated announces, replay high-water marks, EV10 provisional-apply (§11), rekey gated on old-key continuity (§12). |
| **A3 — active MITM present inside the first pairing window** | full protocol MITM at physical proximity during the user's own ~3 s window | **Not defeated — the TOFU residual.** Bounded by: user initiation, proximity, the 3 s window, and *permanence*: the moment the MITM leaves the path every CCM tag fails, the silence guard releases, and the user is forced into a visible re-pair. Undetected compromise requires a permanently resident active attacker. Rekeys are NOT re-exposed (continuity binding, §12). |
| **A4 — downgrade attacker** | strips/forges the capability advert at pairing | **Closed for mislatch** (advert is inside the transcript => confirmation fails, §7). **Advert-strip on a first pairing** degrades to plaintext — the declared TOFU-adjacent residue; once any KX has succeeded, both ends' persisted enc state refuses plaintext with that peer forever (`bond_enc` latch semantics retained, `rf_crypt.h:84-93`). `KBD_REQUIRE_ENC` build flag (default OFF) closes even the first-pair case for all-new fleets. |
| **A5 — DoS / jammer** | deny RF | Out of scope at the PHY. Design guarantee: DoS can never become key compromise, silent plaintext fallback on an encrypted bond, or a wedged state — every failure converges to retry/replug. |
| **A6 — malicious USB host** | full IAP access to the dongle | Sees plaintext HID by design (host is the consumer). Cannot read the link key (BondRead redaction, verified). Key-bearing BondWrite is boot-window/keyless-gated in release builds (§17), so overwrite is replug-gated DoS-until-re-pair, never silent. |
| **A7 — physical possession / debug port** | flash readout | **Not defeated in silicon** — no secure element, bond keys rest in plaintext flash. Production ships CFG_ROM_READ read-protect; residual documented. |

Explicit non-goals: per-session forward secrecy (§9.5), traffic-analysis
resistance (frame lengths reveal report class), host-side confidentiality.

---

## 3. Crypto suite (required point 2 decision)

**Primitives: X25519 (RFC 7748) + AES-128 (existing `hal_aes` engines) +
AES-128-CMAC (RFC 4493) + SP 800-108 KDF in counter mode with CMAC as PRF.
SHA-256 is NOT added.**

Why CMAC wins over adding SHA-256 (unanimous across all three passes):

- Reuses the exact AES seam the stale-abort campaign already hardened: CH592 gets
  the `RF_CRYPT_AES_DOUBLE` double-compute for free, CH570 gets the tuned
  software kernel and the boot-KAT history. SHA-256 is +2-3 KB flash, ~300 B
  state, a second KAT, and a second validation campaign.
- The one thing CMAC cannot do — collision-resistant hashing under a *public*
  key — is engineered out. **Normative invariant:** the transcript is short
  (~103 B, §7), held/streamed verbatim, and MAC'd only under secret keys derived
  from the ECDH output. There is no "transcript hash under a fixed key" anywhere
  in this design; CBC-MAC/CMAC under a known key is trivially collision-forgeable
  and any future change that compresses the transcript under a non-secret key is
  a vulnerability, not a refactor.
- Precedent: this is Bluetooth LE Secure Connections numerology (f5/f6 CMAC
  derivation and confirmation) with X25519 substituted for P-256.

**Constructions (all NIST-citable):**

```
KDF(K, Label, Ctx) = CMAC(K, 0x01 || Label || 0x00 || Ctx || 0x00 0x80)
                     (SP 800-108 counter mode, one 128-bit output block,
                      [L]2 = 0x0080 big-endian, Label = ASCII, no NUL)

Z       = X25519(d_self, Q_peer)          abort if Z == 0 (low-order point)
PRK     = CMAC(0^16, Z)                   SP 800-56C extract, fixed zero salt
T_tag   = CMAC(PRK, T)                    T = transcript, §7
K_cd    = KDF(PRK, "CD", T_tag)           dongle confirmation key
K_ck    = KDF(PRK, "CK", T_tag)           keyboard confirmation key
LK      = KDF(PRK, "LK", T_tag)           the 16 B link key that lands in the bond
K_ann   = KDF(LK,  "AN", <empty>)         announce / SESSION_REQ key (cached, 16 B RAM)
SK      = KDF(LK,  "SK", sid_le4)         per-session CCM key, RAM only (§9)
```

All CMAC/KDF blocks on CH592 route through the doubled backend under
`RF_CRYPT_AES_DOUBLE` — mandatory, since KEX-time AES runs in exactly the
task-context window the 12.3% campaign hit. Retry budget for KEX-time engine
faults adopts the announce pattern (`RF_CRYPT_ANNOUNCE_RETRIES 3`) until the M1
campaign sizes it properly [CAMPAIGN DATA PENDING: aes_redo / announce_retry
rates].

**X25519 requirements:** vendored single-file constant-time 32-bit implementation
(fiat-derived or donna32/c25519 family), RFC 7748 clamping, mandatory all-zero
shared-secret rejection, Montgomery ladder with arithmetic cswap and no
secret-indexed loads (benign on these cache-less cores; SRAM loads are a fixed
2 cycles, `hal_aes.h:120-124`), **explicit yield points** at field-op/ladder-step
granularity, all state in a static work area, <=200 B stack above caller. The
CH570 toolchain already targets Zba/Zbb/Zbc/Zbs — the field arithmetic should use
them.

**KATs:** CMAC RFC 4493 vector added beside the FIPS-197 boot KAT in
`rf_crypt_init()`. X25519 RFC 7748 vector runs at pairing-mode entry only (one
scalarmult is too heavy for every boot; private keys are only minted around
pairing). Host oracles `cmac_ref.py` / `kdf_ref.py` / `kex_ref.py` gate both
firmwares' CI exactly like `ccm_ref.py` does today, with one shared vector file
pinned in both repos.

---

## 4. Wire formats (byte layouts and collision audit)

On-air payload shown from `[ctrl]`; LEN is `rxBuf[1]` (counts from ctrl).
Occupied today: plaintext LENs {1,3,4,7,10,15}; (0xA3,16) (0xA8,19) (0xA1,22)
encrypted HID; (0xA5,14) announce dn; (0xA6,3) advert up.

**Every dongle->keyboard frame is <= 15 B (no CH570 TX DMA change) and every
keyboard->dongle frame is <= 22 B (no keyboard TX buffer change). No radio
buffer grows anywhere.**

```
CAP ADVERT v2   kb->dg   plaintext, pairing slots            LEN 3
  [ctrl][0xA6][version=2]
  (old dongles check version == 1 exactly and ignore this => plaintext bond)

KX FRAME        up tag 0xA7, dn tag 0xA9                     LEN 11  (new LEN)
  [ctrl][tag][hdr][chunk:8]
  hdr = mmmm ffvv : m = msg, f = fragment index 0..3, v = kexver = 1
    m=1 PUB        f=0..3   chunk = X25519 pubkey bytes 8f..8f+7
    m=2 CONFIRM    f=0..1   chunk = confirmation MAC bytes 8f..8f+7 (full 16 B MAC)
    m=3 ABORT      f=0      chunk[0] = reason, rest zero
    m=4 REKEY_REQ  f=0      chunk = trunc8(CMAC(LK_old, "RK" || sid_le4 || dir))   (§12)

SESSION_REQ     kb->dg   keyed, sessionless                  LEN 14  (tag != 0xA5)
  [ctrl][0xAA][chal:u32 LE][mic:8]
  chal = kb_boot_epoch(16) || kb_req_seq(16)   — strictly monotonic across kb reboots
  mic  = CCM-MAC(K_ann, nonce = 0x00000000 || 0x03 || chal_le4 || 0^4,
                 AAD = ctrl||0xAA, body = empty)

ANNOUNCE v2     dg->kb   wire shape byte-identical to today  LEN 14
  [ctrl][0xA5][sid:u32 LE][mic:8]
  mic  = CCM-MAC(K_ann, nonce = sid_le4 || 0x02 || chal_le4 || 0^4,
                 AAD = ctrl||0xA5, body = empty)
  (the freshness challenge is folded into the NONCE, not the frame — the
   prebuilt-buffer ISR TX path is untouched; legacy suite-1 bonds keep today's
   exact construction under LK with chal = 0)

LED RELAY (sealed)  dg->kb                                   LEN 13  (new LEN)
  [ctrl][0xAB][dctr:u16 LE][ct:1][mic:8]
  sealed under SK, nonce = sid_le4 || 0x02 || dctr_le4(zero-extended) || 0^4,
  AAD = ctrl||0xAB; dctr starts at 1 per session, keyboard keeps a high-water
  mark; plaintext LEN-3 relay refused on suite-2 bonds

ENCRYPTED HID   up                                           LEN {16,19,22}
  unchanged wire format; key becomes SK, counter restarts at 1 per session
```

Collision audit: LEN 11 and 13 are brand-new lengths; (0xAA,14) shares LEN with
(0xA5,14) but tags and directions differ and both classifiers route on exact
(tag,LEN) pairs; deliberately **not** using LEN 15 or 17 for any new downlink
(15 aliases the plaintext pair-ACK/EV10 length class; 17 exceeds the CH570 TX
cap). Tolerant parsers ignore unknown (tag,LEN) pairs, so mixed-version
bring-up cannot wedge either side.

**Dongle->keyboard capability hint: CUT (corrected after review, 2026-08-17).**
The draft set bit 7 of the pair-ACK `type_tag` when `peer_capable` was latched,
on the belief that the byte is consumed "only as `% 5` / `% 3` hop seeds" and
that high bits are free. Both reviewers confirmed this is false against the
tree:
- `rf_protocol.h:293,298` compute `type_tag % 3` and `% 5` over the **full
  byte** (channel counts 3 and 5), so bit 7 changes the index: `0x02 % 5 = 2`
  vs `0x82 % 5 = 0`. The two ends would hop on different schedules.
- The shipped keyboard's LEN-15 accept gate is `rxBuf[6] < 5`
  (`../OpenController/firmware/src/rf_task.c:879`), so a `0x82` type_tag is
  **silently dropped** — and that gate guards *every* LEN-15 accept including
  bonded reconnect, so a bit-7 ACK breaks pairing outright for a v2 keyboard,
  which is worse than plaintext.
- The dongle bond has no `type_tag` field and rebuilds `rf_pair_ack15[4]` from
  a compiled constant (`rf_task.c:238-245,3473-3483` patch bytes 0-3/5-14,
  never 4), so the hint could not even survive a dongle reboot — the two ends
  would diverge on hop grid at the next reconnect.
- It is also an active-attacker DoS: flipping bit 7 in one pair-ACK desyncs
  channel selection so the link never forms and the transcript check that
  would catch the tamper never runs.

So the KX pre-arm signal is **only** the first DG PUB fragment (500 ms timeout,
§6); there is no pair-ACK hint. Independently, mask `type_tag & 0x7F` before
both modulos in both repos (`rf_proto_hop_seed`, the pair-scan seed, and the
keyboard's `stored_type_tag`) so any future use of the high bits is genuinely
orthogonal to hopping — a hardening item for the same batch, not a dependency
of KEXv1.

---

## 5. Key establishment: placement and protocol (required point 1)

### 5.1 Why X25519, and why it runs on the connected link

- Release keyboards compile no key-write command and have no USB; an on-air
  production path is mandatory, and against A1 only asymmetric agreement works —
  there is no AES-only construction that survives a passive recording of the
  pairing. PAKE-class schemes need a shared secret or display this hardware does
  not have; P-256 costs more flash and more validation surface for zero gain;
  PQC is out of the flash/RAM class entirely.
- **KX runs after burst-promote, on the 875 us poll grid** — never inside the
  beacon/pair-ACK exchange. The pairing wire behavior stays byte-identical for
  stock keyboards (hard requirement), the connected grid provides the ARQ rhythm
  the announce and app-TX queue already ride, and the DH compute would not fit
  inside the 3 s boot window alongside beacon dwell timing anyway.
- Ephemeral-ephemeral, fresh keys per pairing/rekey attempt. Own keypair is
  precomputed off the critical path: the dongle at boot-window entry, the
  keyboard while beaconing (20 ms slot spacing is >95% idle) — only ONE live
  scalarmult per side remains inside the handshake.

### 5.2 Message flow (fresh pairing)

```
(pairing phase — bytes unchanged)  kb: cap advert v2 + beacons; dg: pair-ACK x6 -> CONNECTED
KX0  dongle: entropy gate (§13). Pool not ready -> plaintext bond + KX_PENDING flag,
     retry at next connect. Never blocks the user.
KX1  dg->kb : PUB f0..f3 (round-robin, one per poll slot, in place of the poll —
              exactly the announce TX discipline)
KX2  kb->dg : PUB f0..f3 (response slots; kb's first fragment implicitly acks KX1)
     [both sides: X25519 sliced in task/main context; polls and plaintext HID keep
      flowing; supervision stays alive because KX frames count as RX activity]
KX3  dg->kb : CONFIRM f0,f1 = CMAC(K_cd, 0x44 || dongle_mac || kb_mac)
              (sent only after the dongle finishes DH+KDF; its appearance
               implicitly acks KX2)
KX4  kb->dg : CONFIRM f0,f1 = CMAC(K_ck, 0x4B || kb_mac || dongle_mac)
              (kb verifies KX3 FIRST, commits candidate key §5.4, then answers;
               idempotently re-answerable)
     dongle verifies KX4 -> commits bond (LK, flags |= CAPABLE|KEY|SUITE2|TOFU,
     KX_PENDING cleared) -> bumps epoch -> waits for SESSION_REQ -> announce ->
     encrypted traffic begins.
```

**Fragmentation / loss handling: round-robin repetition with implicit ACK — no
ACK protocol, no sequence numbers.** Each side cycles the fragments of its
current message, one per available slot, until it observes the peer's next state
(peer's first PUB fragment acks the pubkey; peer's CONFIRM acks ours; the
announce acks CONFIRM_K). The receiver holds a 32 B buffer + 4-bit fragment
bitmap per message. A 4-fragment pubkey completes in ~3.5 ms at 0% loss and
degrades gracefully (~14 ms at 50% loss). Duplicate fragments are idempotent; a
fragment with an out-of-state `hdr` is dropped and counted (`kx_drop[]`
telemetry). Timeouts: **per-state 250 ms, whole-KX 1.5 s**, then
`ABORT(timeout)` and the §8 failure path.

**Grid safety:** nothing KX ever runs in ISR context. Dongle KX TX reuses the
announce publish/consume discipline verbatim (`kx_tx[11]` prebuilt + volatile
live flag consumed by `rf_send_poll`); KX RX rides the existing 2-deep crypt
FIFO (11 B fits its 22 B slots) into a task event. X25519 shares no state with
`hal_aes`, so the BLEB stale-abort hazard does not touch the ladder; slices
yield every <= ~250 us and the TMR ISR poll TX preempts them cleanly on CH570.

### 5.3 State machines

Dongle (responder):

```
KX_IDLE ──burst-promote ∧ peer_capable(v2) ∧ entropy-ready ∧
          (no SUITE2 key ∨ KX_PENDING ∨ rekey-open §12)──────────► KX_RUN
KX_RUN        poll slots cycle DG PUB f0..f3; RX collects KB PUB into bitmap
   ──bitmap full──► KX_CALC
KX_CALC       sliced: Z -> zero-check -> PRK,T_tag,LK',K_cd,K_ck; poll slots
              revert to plain polls during compute
   ──done──► KX_CONF
KX_CONF       slots cycle CONFIRM f0,f1; RX: kb CONFIRM -> verify
   ──verified──► KX_COMMIT: bond.link_key = LK', flags per §5.2, deferred
                 bond_save (existing auto-persist gates) ──► KX_DONE
any state ──state timeout 250 ms / total 1.5 s / peer ABORT / Z==0 /
            engine-fault exhaustion──► KX_FAIL(reason):
     retry once with the SAME ephemerals inside the window; then
       first pair:            plaintext bond + KX_PENDING + diag counter
       re-pair / rekey w/ LK: keep old LK and flags, resume encrypted operation
```

Keyboard (initiator-side mirror):

```
KX_IDLE ──promoted ∧ sent v2 advert ∧ (hint bit7 ∨ nothing yet)──► KX_WAIT
KX_WAIT       500 ms for DG PUB f0
   ├─rx────► KX_COLLECT: buffer D_pub; response slots cycle KB PUB f0..f3
   └─timeout─► legacy path: stock/old dongle — plaintext per policy §8
KX_COLLECT ──bitmap full──► KX_CALC (main-loop slices; keepalives continue)
KX_CALC ──done──► KX_WAIT_CONF: verify dongle CONFIRM
   ──verified──► commit LK' as CANDIDATE (LK_old retained, §5.4), deferred
                 RF_EVT_SAVE_BOND ──► KX_ANSWER
KX_ANSWER     response slots cycle kb CONFIRM until an announce verifies under
              K_ann(LK') — the dongle-commit proof ──► KX_DONE (promote
              candidate, erase LK_old)
failures      same taxonomy: retry once, then plaintext-or-old-key per §8
```

Interlocks (~10 lines each): EV10 mint suppressed while KX active; silence guard
suspended while KX active (KX has tighter timeouts and would otherwise trip the
64-frame guard mid-handshake); KX frames refresh supervision; plaintext HID
during first-pair KX allowed (identical to today's capable-but-keyless state),
refused during rekey of an encrypted bond (existing `bond_enc` latch — typing
pauses <= ~250 ms).

### 5.4 Commit ordering and the last-message problem

- Keyboard commits a **candidate**: on verifying the dongle's CONFIRM it persists
  {LK_new candidate, LK_old retained} (KBD3 dual slot, §15) and answers.
- Dongle commits **last**, on verifying the keyboard's CONFIRM. The dongle is
  always single-keyed; its record never grows.
- If the keyboard's CONFIRM is lost and the dongle aborts: the keyboard holds
  both keys. On the next connect it sends SESSION_REQ under K_ann(LK_new); no
  valid announce within 250 ms -> falls back to K_ann(LK_old), links up,
  schedules a re-KX (KX_PENDING). First verified encrypted exchange under either
  key promotes that key and erases the other. **No deadlock, and no reachable
  state where both ends are keyless.** Power loss at any point degenerates to
  one of these recoverable states.

### 5.5 Per-chip budgets

X25519 figures are literature envelopes for optimized 32-bit implementations —
[MEASUREMENT REQUIRED] via the existing `firmware/validation` cycle harness on
silicon before default-enabling, same discipline as the AES table.

| | CH592 dongle (60 MHz) | CH592F keyboard (60 MHz) | CH570 dongle (100 MHz) |
|---|---|---|---|
| X25519 scalarmult | ~2.5-6 Mcyc => 42-100 ms | same | ~2-5 Mcyc => 20-50 ms (Zba/Zbb in field arith) |
| Slice unit | <= ~15 kcyc = 250 us per executor pass | same, main loop | <= ~25 kcyc = 250 us task slices |
| Whole-KX wall time | ~0.3-1 s sliced (inside the 1.5 s deadline with retransmits) | same | same |
| CMAC/KDF per KX | ~20 blocks, x2 under AES_DOUBLE — sub-ms | same | ~20 x 1,672 cyc ~= 35 kcyc ~= 0.35 ms task |
| Grid impact | **bounded jitter <= one slice** [MEASUREMENT REQUIRED]: poll TX runs in TMOS task context (`hal_timing_ch592.c:6-8`), so a KX slice delays it — same class as today's task-context CCM, not zero (corrected after review) | bounded jitter <= slice: response TX is main-loop, `STOCK_ISR_FAST_RESPONSE=0` — a slice must not sit in the poll->response turnaround (§19 rule) | **zero by construction**: poll TX stays in the TMR ISR, no KX code in ISR — the only chip where the claim holds |
| KX static RAM | ~750 B arena (field elems ~400 + keys/peer/ss 128 + transcript ~103 + FSM/bitmap ~32 + TX bufs) | ~800 B (incl. dual-key staging) | **does not fit today** — the encrypted image is already ~128 B OVER the 0x800 floor (`bench/STACK-WATERMARK.md`, map-verified), not +176 B slack; hard-gated on §14 |
| Stack transient | <= 200 B during a slice [verify via CMD_STACK_WATERMARK] | same | same, re-budget against CH570_STACK_FLOOR |
| Flash | +8-11 KB (curve ~7 KB + CMAC/KDF ~1 KB + FSM ~2 KB) of ~6.3 KB RAM / ample flash | same, of 448 KB | same, of ~30 KB+ free — flash is not the constraint |

---

## 6. Key confirmation and failure UX (required point 3)

Confirmation is the CONFIRM exchange of §5.2: full 16-byte CMACs under keys
derived through `T_tag`, binding transcript and both MAC identities, keyboard
verifying the dongle first. **Neither end records `ENC_KEY`/a committed link key
before its confirmation verifies** — the bond can never hold an unconfirmed key.

| Failure | Behavior | What the user sees | Recovery |
|---|---|---|---|
| RF loss during KX | 250 ms state retries -> 1.5 s abort -> **plaintext bond + persisted `KX_PENDING`** | Keyboard just works (plaintext); no visible error | Automatic: KX re-runs at every (re)connect until it succeeds; encryption activates silently |
| CONFIRM mismatch (MITM or corruption) | abort, count `kx_confirm_fail`, retry once in-window, then plaintext + KX_PENDING | Keyboard works plaintext | Automatic retry next connect; a *persistent* mismatch is visible in `CMD_CRYPT_DIAG` and the support script says "possible interference/attacker — re-pair elsewhere" |
| Entropy pool not ready | KX not offered this connect | none | automatic next connect (§13) |
| Engine fault (double-compute exhaustion) during KX | ABORT frame + same fallback; **never silently derives a wrong key** — a stale abort inside a KDF becomes a bounded retry, exhaustion a clean abort | existing engine-fault telemetry | as above |
| Rekey of an encrypted bond fails | old LK kept, encrypted link resumes | <= ~250 ms typing pause | nothing needed; no downgrade path exists |
| Power loss between commits | keyboard dual-slot resolves at next connect (§5.4) | at worst one reconnect hiccup | automatic |
| Capable keyboard vs CH570 dongle pre-enablement | advert latched, KX compiled out => plaintext + KX_PENDING | works plaintext | documented; encryption arrives with the CH570 enablement release |

**Policy: fail-open to plaintext on FIRST pairing; fail-closed forever after the
first successful confirmation** (encryption, once on for a peer, never comes off
except by explicit re-pair — the existing `bond_enc`/latch semantics, kept).
`KBD_REQUIRE_ENC` (keyboard build flag, default OFF) turns first-pair failures
into refusal for closed fleets; it cannot be the default because a capable
keyboard must keep working against stock dongles.

The universal support sentence: **"unplug the dongle, plug it back in, and put
the keyboard in pairing mode within 3 seconds"** — a full re-pair with fresh KX
resolves every divergence state in this design by construction.

---

## 7. Transcript binding — closing the anonymous-advert mislatch (required point 2)

```
T (103 B) = "OKX1"                                  4   suite/domain label
          || 0xA6 || cap_version                    2   the advert AS LATCHED/SENT
          || beacon bytes, ctrl normalized to 0    10   kb MAC, interval, timeout — the accepted one
          || pair-ACK bytes, ctrl normalized to 0  15   session AA, type_tag, interval, timeout, dongle MAC
          || D_pub                                 32
          || K_pub                                 32
          || cont                                   8   continuity (below)

cont = trunc8(CMAC(LK_old, "CN" || D_pub || K_pub))  if both ends hold a prior LK
       for this peer MAC; else 0^8
```

Both ends already hold every field in state; T is a canonical re-serialization
(ctrl bytes zeroed because ARQ bits vary per retransmit), either buffered
(~103 B) or streamed through CMAC — equivalent, since it is only ever MAC'd
under the secret PRK (§3 invariant). `T_tag = CMAC(PRK, T)` feeds every derived
key, so:

- **Capability is retroactively authenticated.** A mislatched or stripped-then-
  forged advert, a version mismatch, or any tampered interval/timeout/AA/MAC
  produces different T on the two ends => different keys => confirmation fails =>
  abort/retry. The documented anonymous-advert residue (`rf_task.c:1415-1419`)
  closes here, as that comment predicted. Additionally, `ENC_CAPABLE` stops
  being persisted at burst-promote — capability and key are both recorded only
  at KX confirmation, so a mislatch now merely arms a KX attempt that times out
  against a stock keyboard.
- Both MAC identities are inside T => no unknown-key-share; role-distinct labels
  and confirmation constants (0x44/0x4B) => no reflection.
- Pubkey fragments are necessarily plaintext and CRC-only pre-key; any
  corruption or substitution fails confirmation — the transcript MAC is the
  actual integrity guarantee, per-fragment protection is not needed.
- The **continuity field** upgrades every re-pair: a MITM at re-pair time who
  lacks LK_old produces mismatched T => confirmation fails => both ends keep the
  old key. Re-pairing is therefore *stronger* than first pairing — TOFU exposure
  is confined to the very first contact between two units.

Forged advert against a stock keyboard: dongle arms KX, nothing answers, 1.5 s
abort, plaintext + KX_PENDING — the attack buys a retry loop, nothing else.

---

## 8. MITM stance (required point 4)

**Chosen: TOFU at user-initiated physical pairing, upgraded to authenticated
re-pairing by key continuity. No mandatory OOB channel.**

Justification: the dongle has no UI surface; the keyboard module's only side
channel is its host-MCU UART; there is no display, no NFC, no secret-entry
surface to build numeric comparison on, and stock-compat plus the keyboard's
lack of host USB rule out host mediation as the mandatory path. The residual
(A3, §2) is stated, bounded, and honest: undetected compromise requires an
attacker who was present inside the user's own ~3 s proximity window **and stays
on-path forever** — leaving the path forces a visible re-pair.

Optional hardening, explicitly out of the v1 critical path:

- **SAS mode (v1.x, host-assisted):** the dongle sits on USB — a host tool can
  display `SAS = KDF(PRK, "SA", T_tag) mod 10^6` and the user types the 6 digits
  on the keyboard, folded into the keyboard's CONFIRM. Defeats A3 outright when
  a host tool is present; costs zero radio protocol change; cannot be required
  because pairing must work host-software-free.
- **Fingerprint audit (read-only, ~30 lines):** IAP `CMD_KEY_FPR` and a keyboard
  UART twin return `trunc8(CMAC(LK, "FP"))`; the factory/bench compares them.
  Matches the product class's threat model without inventing UX the hardware
  cannot carry.

---

## 9. Session keys and nonce strategy (required point 6)

### 9.1 Hierarchy and what persists where

```
LK      bond record, both ends            per pairing/rekey generation
K_ann   RAM cache (16 B), derived at LK install
SK      RAM only, derived at each mint:  SK = KDF(LK, "SK", sid_le4)
```

Uplink HID seals under SK dir 0x01; sealed LED relay under SK dir 0x02;
announce/SESSION_REQ under K_ann (dir 0x02/0x03 in their nonces). One key per
session for both directions — the CCM direction byte already domain-separates
the halves, and a second per-direction schedule would cost CH570 another 7.6 k
cycles per mint for nothing.

### 9.2 Monotonic session ids kill the birthday residual

```
sid = key_epoch(hi16) || seq(lo16)
```

- `seq`: RAM, starts at 1 per boot, increments per mint.
- `key_epoch`: persisted on the **dongle**, bumped once per boot, at every LK
  change, and on `seq` wrap. Replaces the two xorshift32 draws at
  `rf_task.c:3298` for suite-2 bonds.
- Result: sid is strictly increasing for the lifetime of LK, so **SK never
  repeats and no (key, sid, ctr) triple can ever recur** — the documented
  ~32-bit birthday residual, the keyboard reboot counter-restart residual, and
  `KBD_CRYPT_EXHAUSTED` re-key pressure all become structurally impossible
  rather than probabilistically avoided. The 13-byte nonce layout, wire format,
  and `ccm_ref.py` are untouched.
- Keyboard counter: **monotonic for the lifetime of the key — never restarts
  on `adopt_session`.** (Corrected after adversarial review, 2026-08-17; the
  earlier draft restarted it at 1 per session and both reviewers flagged it as
  a keystream-reuse reintroduction.) `kbd_rf_crypt.h:43-60` documents this as a
  load-bearing invariant precisely because a `sid` can recur: the dongle
  re-announces one session up to 8 times and a re-served REQ (§10) can hand
  back a *still-live* sid, so a per-session restart would seal fresh HID under
  a repeated `(SK, dir, ctr)` and leak keystream. Monotonicity makes
  correctness independent of sid uniqueness — it holds even if the §9.3
  journal-loss reseed ever re-mints a prior `sid`. The counter starts at 1 at
  **key install** and only the per-boot randomized `ctr_start` is retired for
  suite-2 (the 2³¹ headroom drains to a rekey, §12, not to a wrap-restart).
  The sealed LED downlink counter (`dctr`, §11) is monotonic per key on the
  same grounds. Suite-2's `sid` monotonicity (above) is then a *defence in
  depth* layer, no longer the sole guarantee.

### 9.3 Epoch persistence and DataFlash wear

Per-mint persistence is forbidden by arithmetic (mints are ~1/s via EV10; any
per-mint write kills a 10 k-cycle page in days). Instead:

- **Dongle epoch journal — chip-asymmetric placement (corrected after review,
  2026-08-17):**
  - **CH592:** DataFlash 0x75100. `EEPROM_ERASE(off,len)` erases 256 B pages
    (`platform_ch592.c:100-103`), so 0x75100 is a separate erase page and
    `bond_save`'s 256 B erase at the bond offset never touches it. 64 slots
    x 4 B, one slot programmed (no erase) per epoch bump, erase when full: one
    erase per 64 bumps. At one bump/boot plus ~1.3/day from seq wrap =>
    decades against a 10 k spec.
  - **CH570:** the draft placed it at `BOND_EEPROM_OFF + 0x100`, which the
    implementation reviewer confirmed is **inside the bond's own single 4 KB
    erase unit** (`dongle_target.h:93-94`) — and `dongle_nv_erase` ignores
    off/len and erases the whole unit (`platform_ch570.c:453-463`), so every
    `bond_save` would wipe the journal and a journal-full erase would wipe the
    bond. Unimplementable through the current NV seam. The journal must instead
    live in the **spare page at 0x3B000** (OBP-clamped out of OpenBoot's reach,
    `dongle_target.h:89-92`, `link.ld:13`), which requires: extending
    `nv_range_ok` (`platform_ch570.c:400-410`) to admit it, a per-unit erase
    entry point the seam does not yet expose, and empty-slot detection that
    compares against the **descrambled erased-word constant, not 0xFF**
    (`dongle_target.h:96-99`). This is a prerequisite of enabling suite-2 on
    CH570 (it batches with the §14 CH570 gate), not an additive L1 change.
  - `bond_record_t.reserved0` becomes a diagnostic mirror of the epoch (the
    journal is authoritative); `bond.c:105` stops forcing it to zero. Note
    `reserved0` is inside the checksummed span (`bond.c:12-20,115`) so every
    writer and every host tool must set it consistently, and `bond_tuple_equal`
    does **not** compare it (`bond.h:122-151`) — an epoch-only change is
    invisible to the auto-persist skip gate and must not be relied on to
    trigger a rewrite. Old firmware ignores both — see §15 rollback notes.
- **Keyboard boot-epoch journal** (for SESSION_REQ freshness, §10): identical
  64-slot pattern in DataFlash 0x74100 [VERIFY stock keyboard never touches it],
  bumped once per keyboard boot. Keyboards boot rarely; wear is nil.
- The keyboard persists **nothing** per session or per dongle boot.

### 9.4 Mint cost (task context, off-grid — the hal_aes contract)

CH570 worst case per mint: KDF(SK) ~4 blocks + K_ann swap when needed + key
schedule install 7,647 cyc + announce seal 3 blocks ~= **~25 kcyc ~= 0.25 ms**
in task context (~0.03% CPU at 1 Hz mints). CH592: ~5 kcyc. Note the hal_aes
single-schedule constraint: SESSION_REQ verification swaps K_ann in and SK back
out (~2 schedules + 3 blocks ~= 20 kcyc CH570, rate-limited to 1/100 ms);
CH592's engine reloads the key per block anyway, making swaps free there.
[CAMPAIGN DATA PENDING: announce_retry base rate re-baselined once mint =
derive+seal.]

### 9.5 Forward secrecy — honest statement

`SK = KDF(LK, sid)` means an LK compromise exposes recorded sessions of that
generation. Per-session DH is unaffordable (20-100 ms per sub-second EV10 mint).
Exposure is bounded by the rekey lifecycle (§12) and by continuity-ratcheted
generations (fresh Z per rekey — compromise of one generation does not
retro-expose earlier ones). Accepted and documented.

---

## 10. Announce freshness (required point 8)

**Mechanism: challenge echo (solicited announces), zero per-connect persistence.**

- Keyboard, whenever it holds a key but no session (boot, link loss, EV10 hop,
  mint flush): sends `SESSION_REQ` with `chal = kb_boot_epoch || req_seq` in
  its response slot instead of the plain keepalive. It retries the SAME chal for
  250 ms, then bumps `req_seq`. Strictly monotonic across keyboard reboots via
  the boot-epoch journal.
- Dongle, on a MIC-valid SESSION_REQ (task context, rate-limited 1/100 ms, plus
  a RAM chal high-water mark that drops chal <= last-served for free): mints (or
  serves the pre-minted) session, seals the announce with **chal folded into the
  nonce**, publishes to the 8-poll announce window exactly as today.
- Keyboard accepts an announce only if the MIC verifies under its **current
  outstanding chal**. A replayed announce — old sid, old chal, any earlier
  power-cycle — fails the MIC deterministically. **A replayed announce can never
  desync the keyboard, period**, versus today's design note where replay is
  accepted and costs a silence-guard round-trip; the worry-case (rebooted
  keyboard adopts a stale sid, resets counters, reuses keystream) is
  structurally eliminated because adoption is always gated on a fresh challenge
  and SK is unique per sid anyway.
- A replayed SESSION_REQ makes the dongle mint a session the keyboard ignores —
  rate-limited nuisance, bounded, converges via silence guard + re-request in
  <~100 ms.
- Persistence cost: one 4 B slot per keyboard boot (§9.3). Nothing else.
- EV10 interplay: after an EV10 hop the keyboard has no session on the new
  parameters and simply REQs there; the dongle pre-mints at EV10 as today but
  **seals the announce per-REQ** (fresh chal), and never re-announces a
  displaced sid.
- Legacy suite-1 bonds (bench keys, un-upgraded): today's fire-and-forget
  8-poll broadcast behavior is kept bit-exact, selected by the bond suite flag.

---

## 11. Downlink protection (required point 7)

The controlling constraint: on CH570, `rf_send_poll` IS the TMR ISR — the
product-wide rule "no crypto in ISR context" (hal_aes non-reentrancy + phase
grid) is met **by construction** in this design: net new ISR-context crypto on
either chip is **zero**. Everything TX'd from the ISR is a prebuilt buffer.

| Frame | Disposition | Rationale |
|---|---|---|
| **Poll, LEN 1** | **Plaintext, out of scope — rigorously.** | (a) must stay plaintext for stock keyboards; (b) content is 1 byte of ARQ state — zero confidentiality value; (c) forging it yields ARQ/phase disturbance strictly weaker than jamming (A5), and injected polls cannot elicit anything replayable (uplink is counter-protected); (d) authenticating it needs ~4-5 AES blocks per 875 us slot in the CH570 TMR ISR (~8-20% of every slot, in a context where hal_aes is forbidden) or permanent task-context pre-sealing of ctrl variants — grossly disproportionate to a DoS-equivalent threat. The silence guard bounds sustained manipulation to a ~56 ms release + reconnect. |
| **Session announce, LEN 14** | Authenticated (already) + **challenge-fresh** (§10). | Wire shape unchanged; prebuilt/ISR path untouched. |
| **LED relay, LEN 3 -> sealed LEN 13 (0xAB)** | **Encrypted when the bond runs suite 2.** | Sealed under SK dir 0x02 with its own u16 counter + keyboard HWM, built in task context on LED *change* (host-driven, rare: ~10 kcyc CH570 / ~4 kcyc CH592), consumed by the ISR from a published buffer — the identical announce discipline. Closes caps-lock state leakage and LED spoofing. Plaintext LEN-3 refused on suite-2 bonds (mirrors the uplink `plain_drop`). On dctr exhaustion (65,536 LED changes in one session) force a re-mint. |
| **EV10 re-key, LEN 15** | **Stock shape untouched (compat-mandatory) + keyboard-side provisional apply.** | A suite-2 keyboard applies forged-able new AA/interval/timeout *provisionally*, REQs on the new parameters, and reverts to bonded parameters if no announce verifies within 64 slots. The attacker can forge the LEN-15 but not the keyed announce, so an unauthenticated link-redirect becomes a <= 64-slot excursion. Zero new bytes on air, ~10 lines + one timer on the keyboard. |
| **Pair-ACK, LEN 15** | Plaintext by stock-compat necessity; retroactively bound in the KX transcript (§7). | Tampering at pairing => confirmation failure. |
| **KX frames** | Self-protecting (confirmation) / continuity-gated (rekey). | §5, §12. |

---

## 12. Rekey lifecycle for the long-term link key (required point 5)

Neither part has a wall clock; "rotate yearly" is undeliverable on-device.
Triggers, in order of expected frequency:

1. **User re-pair** — the universal recovery. Full KX from scratch; continuity
   field (§7) binds it to LK_old when both ends still hold one.
2. **Automatic, silent re-KX on the live link** when: `key_epoch` has advanced
   >= 4096 since the last KX (a boots+wraps age proxy needing no new
   persistence); or KX_PENDING / candidate-key residue exists (§5.4 cleanup);
   or the keyboard signals key exhaustion (kept as belt; practically
   unreachable under per-session counters).
3. **Host-initiated:** new IAP `CMD_KX_REKEY` — forces trigger 2 on next
   connect; the support tool's "rotate keys now" button. `CMD_CRYPT_DIAG` gains
   a `rekey_recommended` bit past the epoch threshold.

**Mechanism:** the same KX frames and FSM over the live CONNECTED link, opened
by `REKEY_REQ` (msg 4, either direction) whose chunk is
`trunc8(CMAC(LK_old, "RK" || sid_le4 || dir))` — bound to the current session id
(fresh, monotonic), so replaying it is idempotent nuisance at worst. The rekey
transcript replaces the pairing fields:

```
T' (92 B) = "OKXR" || dongle_mac || kb_mac || sid_le4 || D_pub' || K_pub'
          || trunc8(CMAC(LK_old, "CN" || D_pub' || K_pub'))
```

A MITM without LK_old can neither open a rekey nor pass its confirmation —
**rekey is MITM-proof, strictly stronger than first pairing**, and each
generation ratchets (fresh Z', so compromise of LK' does not expose pre-rekey
recordings).

**In-flight sessions:** the current session keeps carrying traffic under the old
SK through the entire re-KX (KX frames and HID share the grid exactly as at
first pairing — the user never loses keystrokes). On dongle commit: epoch bump,
mint, announce under K_ann(LK_new); the keyboard's dual-slot logic (§5.4) makes
the cutover glitch <= one announce round-trip. The displaced key is erased on
both sides at first verified traffic under the new one. Power loss mid-rekey
resolves through the dual-slot fallback — automatic, invisible.

---

## 13. Entropy (prerequisite; honest budget)

The current generator is a xorshift32 over UID^RTC(^SRAM-PUF on CH570) — a
<= 2^32 searchable seed space, documented in-tree as near-deterministic on the
bench. **Fine for sids and AAs; it must never feed an X25519 scalar.**

Design: per-device CMAC-conditioned entropy pool (16 B state):
`pool = CMAC(K_pool, pool || sample || src_id)`, `K_pool` from the chip UID
block. Sources:

- **Cold-boot SRAM image**, absorbed before `.bss`/stack init — CH570 already
  captures boot entropy (`rf_ch570_boot_entropy`); the CH592 dongle and the
  keyboard need the equivalent pre-clear hook [WORK ITEM]. Fortunate alignment,
  preserved deliberately: dongle pairing *requires* a replug, so first-pair
  keygen always sees a fresh cold SRAM image.
- **Clock-domain jitter:** low bits of RTC32K-vs-SysTick skew sampled at every
  radio IRQ during pairing and connected operation (beacons every 20 ms while
  pairing, polls at 875 us connected — hundreds of samples inside any pairing
  window; physically independent oscillators).
- **User timing:** SysTick at the pairing chord / replug instant.

Draw policy: a private scalar is squeezed (CMAC-expand, two blocks, then RFC
7748 clamp) only when the conservative credit estimator (1/4 bit per jitter
sample; SRAM image credited only after measurement) reaches **256 bits**;
otherwise KX defers to the next connect via KX_PENDING — invisible to the user,
never a blocked pairing. Reseed before every keygen. No hot-path flash writes;
an optional 16 B carry-over seed may piggyback on the per-boot epoch slot if
measurement demands it.

**Release gates [CAMPAIGN DATA PENDING]:** (a) pool-credit counter added to
`CMD_CRYPT_DIAG` and fill-rate measured during real pairings on the UART bench;
(b) SP 800-90B-style min-entropy estimation over repeated SRAM captures across
units/reboots/temperatures on both dies. Until measured, warm-entry keyboard
keygen (pairing by key-chord without a reboot) is the top residual risk after
TOFU, and the fail-stop stands: no threshold, no keygen.

---

## 14. CH570 enablement gate

Map-verified reality (corrected after review): the encrypted CH570 image is
already ~128 B **over** the production 0x800 floor before any KX state (it links
only under the bench 0x700 override); the KX arena needs ~750 B. So CH570 needs
the stack-floor reclamation *just to keep its current L1 symmetric state*, and
much more before KX. Decision (unanimous across all three passes):

- **KX ships on the CH592 dongle and the keyboard first. CH570 builds with
  `DONGLE_KX=0`:** v2 adverts latch capability and set KX_PENDING but no KX
  frames are ever sent; bonds stay plaintext; stock behavior byte-identical.
  An honest, supportable state — "CH570 dongles: encryption in release N+2" —
  stated in the Makefile as a hard gate, not a TODO.
- Everything else in this design **does** run on CH570 in phase 1: per-session
  key derive at mint (~0.25 ms task), announce v2 + SESSION_REQ verify, sealed
  LED, epoch journal — total new static RAM **~40 B** (K_ann cache 16 B + chal/
  epoch state ~12 B + LED buffer/flags ~12 B), re-verified against
  `CMD_STACK_WATERMARK` before merge. Zero new ISR work.
- **KX enables on CH570 only after the planned stack-floor/RAM-ledger
  reclamation lands** with map + watermark evidence of >= ~800 B static and
  <= 200 B transient headroom under KX + IAP + RX worst case. The crypt-FIFO/
  announce/app-TX overlay was sized (~80 B) and cannot cover the arena; the
  stack-floor work is the real gate and this design does not pretend otherwise.
  Until then, CH570's key route remains USB BondWrite (bench/factory, §17).

---

## 15. Bond records and migration (required point 9)

### 15.1 Dongle — record stays 48 B, version stays 2

Additive within existing spare space; **no layout change, no version bump**:

- `reserved0` u16 -> `key_epoch` diagnostic mirror (journal authoritative);
  `bond.c:105` stops zeroing it.
- New flag bits: `0x04 BOND_FLAG_ENC_SUITE2` (per-session keys + fresh-announce
  semantics active), `0x08 BOND_FLAG_KX_PENDING` (capable peer, key not yet
  established), `0x10 BOND_FLAG_ENC_TOFU` (key provenance: on-air KX vs
  host-provisioned; diagnostic).
- Why not v3: an old firmware loads this record fine (checksum covers it, the
  semantic validator ignores both fields), so **A/B slot rollback keeps the pair
  working**. Un-flagged and legacy-flagged records run today's code paths
  bit-for-bit. `BOND_RECORD_MAX_NV` and both platform scratch buffers untouched.
- Rollback caveat (documented, detected, recoverable): firmware rolled back
  across the suite-2 boundary on a SUITE2 bond runs static-LK announce
  semantics against a per-session keyboard -> MAC-fail loop with a
  `drop_reason[DROP_MAC]` + silence-guard signature -> recovery is the §6
  sentence (re-pair) or `CMD_BOND_CLEAR`. The epoch journal is ignored by old
  firmware; if a rollback event erased it, the next new-firmware boot re-seeds
  `key_epoch = max(journal, random16 | 0x8000)` — collision odds across that
  one event are far below the fully-random scheme this replaces.

### 15.2 Keyboard — "KBD3" v3, ~60 B in its 256 B page

v2 fields + `prev_link_key[16]` + `key_state` u8 (current/candidate discipline,
§5.4) + `boot_epoch` mirror u16 + suite/flag bits. The keyboard keeps its
existing **no-migration rule**: a v2 record is invalidated -> re-pair
(`OpenController rf_task.c:102-107`). That asymmetry is correct and stays:
dongle-side in-place tolerance preserves the bond across dongle IAP updates
(dongles update often, in place); a keyboard version bump forces exactly one
user-initiated re-pair, and its bond is entirely reconstructible by pairing.
Bench units with provisioned v2 keys re-provision at the bench flag day (§16).

### 15.3 Compat matrix (every reachable field state)

| Keyboard \ Dongle | Stock dongle | Shipped OSS dongle (today) | New CH592 dongle | New CH570 dongle (ph. 1) |
|---|---|---|---|---|
| Stock keyboard | plaintext | plaintext | plaintext (no advert => KX never offered; pairing byte-identical) | plaintext |
| Today's OSS kb (v1 advert, bench key) | plaintext | v1 static-key CCM (bench) | v1 advert => **legacy suite**: today's exact static-LK behavior, no KX offered (v1 != v2) | legacy suite |
| New kb (v2 advert), no bond | plaintext | plaintext (version byte != 1 ignored — code-verified `rf_task.c:1813`) | **KX => encrypted, suite 2** | plaintext + KX_PENDING (until L4) |
| New kb, established SUITE2 bond | — | (rollback case, §15.1: MAC-loop -> re-pair) | encrypted, suite 2 | n/a until L4 |

No cell is broken; every cell is at worst plaintext-stock-equivalent, and no
upgrade order of the two repos can brick a link that a re-pair cannot fix.

---

## 16. Two-repo landing plan (each stage shippable alone)

v1 link-crypto never shipped outside the bench, so the only true flag day is on
the bench.

| Stage | Content | Repos / order | Why order-independent |
|---|---|---|---|
| **L0** | Shared constants (tags 0xA7/0xA9/0xAA/0xAB, LEN 11/13/14, CAP v2, KDF labels, transcript layout), tolerant unknown-(tag,LEN) parsing audit, host oracles `cmac_ref.py`/`kdf_ref.py`/`kex_ref.py` + one cross-repo vector file pinned in both CIs | both, any order | no behavior change |
| **L1 (CH592)** | `aes_cmac.c` + `rf_kdf.c` (shared verbatim, like `rf_crypt.c` today) + entropy pool + CMAC boot KAT + SRAM-capture hooks; per-session SK, `epoch||seq` sids, CH592 epoch journal (0x75100), SESSION_REQ/announce v2 (new dongle uplink control-frame classifier + empty-body MAC-verify verb, see §19 open item), sealed LED, EV10 provisional-apply — **all gated on the SUITE2 flag** | both, any order | dormant without the flag; wire unchanged for unflagged bonds. **Lands the nonce-reuse fix before and independent of X25519.** CH570 excluded here — its RAM baseline is negative (see next row). |
| **L1b (CH570)** | Same content on CH570, gated on the CH570 stack-floor reclamation (§14) AND the spare-page (0x3B000) journal relocation (§9.3) | dongle only | the CH570 encrypted image is already ~128 B over the 0x800 floor (`bench/STACK-WATERMARK.md`); L1 on CH570 is NOT the +40 B additive change the draft claimed — it depends on the same campaign as L4 and forces a second CH570 build-id/matrix event. |
| **L2** | X25519 vendored + KAT, KX FSMs + frames, flags-set-at-confirm (incl. moving ENC_CAPABLE persist to confirm), keyboard KBD3 dual-slot record, `DONGLE_KX=1` on CH592 dongle. (No pair-ACK type_tag hint — cut, §4; the DG PUB is the only KX signal. Optional `type_tag & 0x7F` masking hardening lands here.) | dongle-first preferred; either order safe | kb-first: no DG PUB => KX_WAIT times out to legacy. dongle-first: v1 advert => KX never offered. **Bench flag day here:** bench pairs update together and re-provision (documented in README-link-encryption.md). |
| **L3** | REKEY_REQ live rekey + `CMD_KX_REKEY` + `rekey_recommended`; optional `CMD_KEY_FPR` fingerprint audit; SAS host-tool mode if wanted | both | suite-2-gated, additive |
| **L4** | CH570 `DONGLE_KX=1` after the stack-floor reclamation, with map + watermark evidence (§14) | dongle only | pure enablement, no protocol change |
| **L5** | Release hardening: defaults on; `check_bench_key_absent.py` extended to assert `KBD_CRYPT_BENCH_KEY` and `DONGLE_CRYPT_BENCH_FORCE_KEY` compiled out and no UART key-write symbol in release images of **either** repo; key-bearing BondWrite boot-window gate (§17); CFG_ROM_READ read-protect in production | both | — |

Bench-gated on the UART rig at every stage (probe map and recipes per the
existing bench docs). [CAMPAIGN DATA PENDING: M1 aes_redo/announce_retry rates
gate two L2 decisions — the KDF retry budget and whether CONFIRM frames need an
announce-style rebuild-retry.]

---

## 17. Provisioning path disposition (required point 10)

- **`bench/provision_link_key.py` + dongle `CMD_BOND_WRITE` key import + keyboard
  UART 0xAE remain factory/bring-up only, unchanged and labeled as such.** The
  UART command is already compiled out of release keyboards; BondRead redaction
  + redacted-write-back rejection (0xB3) are already live. Records written by
  the tools carry `ENC_KEY` without `ENC_TOFU`; the tool gains `--suite2` to set
  the SUITE2 flag on both ends so the bench can exercise L1 without KX. A
  host-provisioned LK feeds K_ann/SK derivation identically, so the bench path
  keeps exercising the exact production session machinery.
- **The production key path is exclusively the on-air KX.** No host in the loop,
  no key ever on a USB wire, and the established LK cannot be exfiltrated over
  the provisioning interface it replaced (redaction, verified).
- **Release tightening:** release dongle firmware accepts a key-bearing
  BondWrite only while no ENC bond exists **or during the boot window** —
  converting A6's overwrite from silent to replug-gated (still DoS-only either
  way). BondWrite otherwise remains the bond/identity and support-recovery tool.
- **Factory EOL flow:** flash both units -> RF fixture -> trigger pairing inside
  the boot window -> KX runs exactly as in the field (TOFU is safe in a
  controlled fixture) -> fixture reads `CMD_CRYPT_DIAG` and passes on
  `ok_count > 0 && kx_confirm_fail == 0 && ENC_TOFU set`. No key material ever
  leaves the two chips; the factory never knows LK.
- **Support tool (`dongle-ctl`, productized bench scripts):** diag counters,
  redacted bond read, factory-reset (`bond_clear` + reset), `CMD_KX_REKEY`,
  key fingerprint. Support playbook: (1) the §6 replug-and-re-pair sentence;
  (2) `diag` — climbing `drop_reason[DROP_MAC]` = key/session divergence (step
  1 again), static `conn_rx` = radio/placement, not crypto; (3) factory-reset +
  step 1; (4) never key entry, key files, or firmware downgrades; (5) benign
  states to reassure about: "encryption pending" (KX_PENDING after a noisy
  pairing — self-heals next replug), CH570 pre-L4 is plaintext by design.

---

## 18. Consolidated per-chip budget summary

| Resource | CH592 dongle | CH592F keyboard | CH570 phase L1 | CH570 phase L4 |
|---|---|---|---|---|
| Flash added | ~9-11 KB | ~9-11 KB (of 448 KB) | ~2-3 KB (CMAC/KDF/session/announce/LED) | +7-8 KB (curve+FSM) — flash is plentiful |
| Static RAM added | ~800 B (KX arena + session/chal state) vs ~6.3 KB free | ~850 B (incl. dual-key staging) vs ~8.6 KB free | **L1b, NOT +40 B additive** — CH570 is ~128 B over the floor today; gated on §14 reclaim | ~800 B — **hard-gated on stack-floor reclaim** |
| ISR-context crypto added | **0** | **0** | **0** | **0** |
| Worst task-context slice | ~250 us (KX ladder) | ~250 us | ~0.25 ms (mint derive+seal), ~0.2 ms (REQ verify, 1/100 ms cap) | ~250 us slices |
| Poll-grid impact | **bounded jitter <= slice** [MEASUREMENT REQUIRED]: task-context poll TX | bounded: main-loop response, keep slices out of the poll->response turnaround | none (TMR ISR poll TX preempts task slices) | none |
| Flash-write cadence | 1 journal slot/boot (+wraps), erase/64; bond on pair/rekey | 1 journal slot/kb-boot; bond on pair/rekey | same as CH592 dongle | same |
| One-off costs | X25519 keygen at boot-window entry (idle-sliced); KX wall ~0.3-1 s once per pairing/rekey | keygen while beaconing (idle) | — | same as CH592 |
| Stale-abort exposure | all CMAC/KDF under AES_DOUBLE (x2 cost, us-scale); retry budgets [CAMPAIGN DATA PENDING] | same (seal_redo path) | none (SW AES cannot abort) | none |

---

## 19. Open items and campaign dependencies

1. **[CAMPAIGN DATA PENDING]** M1 stale-abort rates (`aes_redo`,
   `announce_retry`) — size the KEX-time KDF/CONFIRM retry budgets; re-baseline
   announce retries once mint = derive+seal.
2. **[CAMPAIGN DATA PENDING]** Entropy: pool fill rate during real pairings;
   SP 800-90B-style min-entropy of cold-SRAM captures across units/reboots/
   temperatures on both dies. Release gate for L2.
3. **[MEASUREMENT REQUIRED]** X25519 cycle/stack numbers on both dies via
   `firmware/validation` before default-enable; slice-length audit against the
   875 us grid.
4. **[MEASURE]** CH570 watermark with the L1 +~40 B state; keyboard keepalive
   stability re-baselined under per-session keys before the sealed-LED consumer
   lands.
5. **[VERIFY]** journal pages (CH592 dongle 0x75100, keyboard 0x74100)
   untouched by stock firmware and OpenBoot on all parts. (CH570 journal moved
   to 0x3B000 per review — §9.3.)
6. X25519 vendoring choice (fiat-derived vs donna32 vs c25519) + RFC 7748/4493
   vectors into both host suites and the pairing-entry KAT.
7. `KBD_REQUIRE_ENC` default per product SKU (design default: OFF).
8. CH592 dongle + keyboard cold-SRAM entropy capture hooks [WORK ITEM].
9. **[NEW — from review]** SESSION_REQ (solicited announce) needs a new dongle
   *uplink control-frame* RX classifier and an empty-body CCM-MAC-verify verb —
   the connected classifier today only routes encrypted-HID shapes, plaintext
   HID, and LEN-1/3/10 (`rf_task.c:1879` + `rf_crypt_encrypted_body_len`), with
   no keyed sessionless-frame path. This is real new dongle RX code with a
   rate limit and chal high-water state, not the "~40 B, zero new ISR work" the
   L1 line implied. Scope it before L1 announce-freshness lands.
10. **[NEW — from review]** State the KDF-vs-direct-CMAC domain separation
    normatively (no KDF label may begin with `0x01`; every direct-CMAC message
    under a shared key must be prefix/length-disjoint from KDF inputs) and add a
    cross-input collision assertion to `cmac_ref.py`/`kdf_ref.py`.
11. **[NEW — from review]** EV10 provisional-apply (§11) must snapshot the
    adopted session + counter on excursion and restore them verbatim on revert,
    and must never seal HID under an un-announced provisional session — else a
    forged EV10 becomes a counter-reset primitive. Rate-limit provisional hops.
12. **[NEW — from review, §5.4 text]** The keyboard cannot pick which LK the
    dongle answers under; the "fall back to `K_ann(LK_old)`" step means *try
    verifying the returned announce under both K_ann candidates*, doubling
    announce-verify cost only during the ambiguity window. Fix the §5.4 wording.

---

## Appendix A — Decisions and alternatives rejected

Each entry names the conflict, the winner, the loser(s), and why.

1. **Transcript/KDF primitive: AES-CMAC (SP 800-38B/800-108); SHA-256 rejected.**
   Unanimous. SHA-256 buys unkeyed collision resistance this protocol never
   needs (every transcript MAC runs under a fresh secret key), at +2-3 KB flash,
   ~300 B state, a second KAT, and a second stale-abort validation campaign.
   Normative guard retained from the security-first pass: the transcript is
   never compressed under a non-secret key.

2. **Key agreement: ephemeral X25519; alternatives rejected.** Clear key
   transport at pairing (industry status quo) loses to a passive recorder
   forever — the MouseJack class. AES-only PAKE needs a per-unit factory secret
   plus entry UX that open-hardware kits cannot anchor. P-256: more code, more
   footguns, no benefit. PQC: out of the resource class.

3. **Downlink payload cap: keep <= 15 B; migration-first's `pair_ack_tx_dma`
   17->24 B growth rejected.** (C lost.) +7 B of static RAM in the tightest
   region on the tree, in a buffer consumed by the TMR ISR, purely to carry
   16-byte fragments that 8-byte fragments deliver ~1.75 ms slower. No radio
   buffer changes anywhere is a stronger invariant than a slightly faster
   handshake.

4. **KX frame shape: single LEN-11 frame, 8 B chunks, both directions
   (implementation-first).** Security-first's asymmetric dn-11/up-19 split lost
   (a second shape and a (0xA7,19)-vs-(0xA8,19) tag-disjointness obligation to
   save ~1.75 ms once per pairing); migration-first's LEN-19 both ways lost with
   decision 3.

5. **ARQ: round-robin repetition with implicit ACK (implementation-first).**
   Security-first's explicit-ack selective repeat lost: sequence/ack machinery
   and per-message echo state buy nothing at 32-byte message sizes on a
   875 us grid where the peer's state advance is already a perfect ACK. The
   4-bit fragment bitmap (reassembly) is kept; the ACK protocol is not.
   Timeouts adopt the tighter 250 ms/1.5 s (B) over 500 ms/2 s (C).

6. **First-pair failure policy: fail-open to plaintext with persisted
   KX_PENDING auto-retry; fail-closed forever after first confirmation
   (migration-first). Security-first's release-default refuse-plaintext lost.**
   A capable keyboard must work against stock dongles, so refusal cannot be
   default; and a fail-closed first pair converts every noisy-RF pairing into a
   dead keyboard to defend against an attack (advert-strip) that only works
   in-person, during the user's own window, against a never-encrypted pairing,
   and that the continuity latch permanently forecloses after one success.
   Security-first's stance survives as `KBD_REQUIRE_ENC` (default OFF) and as
   the never-downgrade latch, which all three designs shared.

7. **Dongle bond record: stays v2/48 B with flag bits + `reserved0` reuse
   (implementation-first + migration-first). Security-first's v3 48->64
   widening lost.** The widening touches `BOND_RECORD_MAX_NV` plus both platform
   scratch buffers, breaks A/B rollback on every fielded dongle, and buys fields
   (epoch, gen, suite) that fit in existing spare space. The epoch journal
   (migration-first) beats storing the epoch in the record (per-boot record
   rewrites) on wear arithmetic.

8. **Keyboard record: KBD3 dual-slot candidate/current keys (migration-first).**
   Security-first's single-commit lost (power loss between commits => visible
   forced re-pair); implementation-first's flag-only layout lost (a lost final
   CONFIRM leaves committed-vs-uncommitted key divergence that only a re-pair
   clears). The dual slot makes every interruption self-healing and gives the
   rekey cutover its glitch-free dual-key trial window. Cost: one keyboard
   version bump under its own established invalidate-and-re-pair rule.

9. **Session keys: ONE key per session, direction separated by the CCM nonce
   dir byte (implementation-first). Per-direction keys (security-first,
   migration-first) lost:** a second 7.6 kcyc key schedule per mint on CH570
   and 16 B more state for a separation the nonce layout already provides.

10. **Session-id freshness: monotonic `epoch||seq` (all three) + challenge-echo
    announce adoption (migration-first mechanism, security-first cookie
    concept). Implementation-first's persisted-epoch acceptance policy lost**
    (keyboard-side flash write per dongle boot, and a keyboard-reboot
    acceptance residual the challenge closes for free). **Security-first's
    second "mid-session chained-MAC" announce mode lost:** solicited-only
    announces cover the EV10 case with one ~few-ms round trip instead of a
    second verification mode. The challenge rides the nonce, keeping the
    announce wire and ISR path byte-identical (migration-first's key insight).

11. **Rekey: continuity-gated live re-KX (migration-first REKEY_REQ, subsuming
    security-first's channel-bound KDF via the transcript continuity field).
    Implementation-first's "no autonomous rekey in v1" lost:** LK is a
    long-lived KDK whose compromise exposes every recorded session of its
    generation; with the KX FSM already on board, rekey costs one message type
    and one transcript variant, and its old-key gate makes it *stronger* than
    first pairing. Kept out of the first landing stage (L3) so it never blocks
    the core path.

12. **LED relay: sealed LEN-13 0xAB under SK (implementation-first shape).**
    Security-first's LEN-15 lost (sits exactly on the cap and aliases the
    plaintext LEN-15 class); migration-first's LEN-17 lost with decision 3.
    u16 counter suffices (LED changes are host-driven and rare; exhaustion
    forces a re-mint).

13. **Polls: plaintext, out of scope. Unanimous.** One byte of ARQ state, a
    forgery value no better than jamming, and any authentication lands multiple
    AES blocks per 875 us slot in the CH570 TMR ISR where crypto is forbidden
    by product rule. The CH570 ISR-context budget for this whole design is
    zero, by construction.

14. **EV10: stock shape untouched + keyboard provisional-apply/revert
    (security-first hardening grafted onto the shared out-of-scope rationale).**
    Pure out-of-scope (implementation-first/migration-first) partially lost:
    the revert timer costs ~10 keyboard lines and shrinks a forged-redirect
    excursion to a deterministic 64-slot bound instead of relying on
    silence-guard convergence alone.

15. **MITM: TOFU + continuity; mandatory OOB rejected (unanimous — no UI
    surface exists).** Security-first's host-tool SAS kept as optional L3+,
    implementation-first's read-only key fingerprint kept as optional audit.
    Both are additive and neither gates pairing.

16. **Confirmation MAC strength: full 16 B both directions, carried as two
    8 B fragments (synthesis).** Security-first's 12 B truncated downlink
    confirm lost (was only forced by its LEN-14 single-frame choice);
    implementation-first's 8 B confirms lost (2^-64 online is defensible for a
    rate-limited handshake but 2^-128 costs exactly one extra 875 us slot).

17. **KX placement: post-promote on the connected poll grid. Unanimous.**
    Pairing-phase bytes stay stock-identical; the grid supplies the ARQ rhythm;
    compute never fits the boot window alongside beacon timing.

18. **Entropy: CMAC-conditioned pool + cold-SRAM capture + clock-jitter +
    credit-gated draws with KX_PENDING deferral (synthesis of all three);
    security-first's full SP 800-90A CTR_DRBG framing simplified** to the
    pool + CMAC-expand construction (equivalent strength for this use, less
    machinery), while keeping its two campaign gates (90B-style SRAM
    measurement; refuse-keygen fail-stop) and its boot-window cold-image
    observation.

19. **Provisioning: bench path frozen as factory-only; production = KX only.
    Unanimous.** Grafted: security-first's boot-window gate on key-bearing
    BondWrite; implementation-first's `--suite2` bench flag; migration-first's
    factory EOL flow, `dongle-ctl`, and support playbook.

20. **CH570: KX deferred behind the stack-floor project; the symmetric state
    ships in stage L1b, also gated on that project. Unanimous on the deferral;
    corrected after review** — the CH570 image is ~128 B *over* the floor
    already (not +176 B slack), so even the symmetric L1 state needs the
    stack-floor reclamation. Shipping per-session keys + fresh announces there
    is still strictly stronger than waiting for full KX, but it is L1b, not the
    additive L1 the draft assumed.
