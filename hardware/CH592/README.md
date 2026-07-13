# OpenDongle CH592

OpenDongle CH592 is a compact USB Type-A 2.4 GHz receiver built around the WCH CH592D RISC-V wireless MCU. The PCB is 16.35 mm x 9.90 mm and uses a PCB-edge USB connector and a WCH-derived 2.4 GHz PCB antenna.

## Design files

| File or directory | Purpose |
| --- | --- |
| `OpenDongle-CH592.kicad_pro` | KiCad 10 project and design-rule settings |
| `OpenDongle-CH592.kicad_sch` | Schematic |
| `OpenDongle-CH592.kicad_pcb` | PCB layout |
| `lib/OpenDongle.kicad_sym` | Project-owned CH592D and four-pin PCB USB symbols |
| `lib/OpenDongle.pretty/` | Project-owned USB edge connector and WCH antenna footprints |
| `sym-lib-table`, `fp-lib-table` | Portable project-local library configuration |

Open the project with KiCad 10 or later. The library tables use `${KIPRJMOD}` paths, so the project resolves every symbol and footprint from this directory and does not depend on global KiCad configuration. Nothing needs to be added to your global symbol or footprint library tables.

## Electrical design notes

- USB VBUS (5 V) feeds U2, an XC6206P332MR-G LDO, whose 3.3 V output supplies the CH592D VIO33/VDD33 pin (pin 5). C1 (1 uF) is the LDO input capacitor and C2 (1 uF) is its output capacitor.
- Y1 is a 32 MHz crystal on X32MI/X32MO (pins 13 and 12). No external load capacitors are fitted; the design relies on the CH592 programmable internal crystal load capacitance.
- The RF output (ANT, pin 15) connects directly to the WCH-derived PCB antenna.
- USB data connects to the CH592D native USB pins: D+ to PB11/UD+ (pin 10) and D- to PB10/UD- (pin 11).
- AE2 and J1 represent copper footprints fabricated as part of the PCB. They are intentionally excluded from the assembly BOM and position file (`in_bom no`, `in_pos_files no`), so they do not appear in the Fabrication Toolkit output as unsourceable line items.
- GND reaches the CH592D through the exposed pad (pin 21).
- The nine unused GPIOs (pins 1, 2, 7, 8, 9, 17, 18, 19, 20) carry no-connect flags.

### The internal DC-DC is deliberately not used

Two parts of this schematic look like mistakes and are not. **Do not "fix" them.**

**VSW (pin 4) is tied directly to VDCID (pin 3), with VDCIA (pin 16) on the same net, and there is no inductor on the board.** This is the configuration the datasheet specifies when the DC-DC converter is disabled.

VSW is the internal power path from VDD33 when the DC-DC is bypassed, so it must **not** be left floating. The decoupling values follow the datasheet's DC-DC-disabled recommendations:

| Pin | Datasheet, DC-DC disabled | This design |
| --- | --- | --- |
| VDCID (3) | "0.1uF or more" | C4, 1 uF |
| VINTA (14) | "0.47uF is recommended" | C5, 470 nF |
| VDD33 (5) | "1uF is recommended" | C2, 1 uF |

The DC-DC is off by default at power-on and is simply never enabled in firmware. WCH's own CH592F reference designs use the *other* branch of this note (a 10 uH inductor from VSW to VDCID), which is why they look different.

### PB15 (pin 6) is tied to GND on purpose

**PB15/TCK/MISO_/SCL is a boot strap, not a stray ground.** Holding it low forces a unflashed part to come up in ISP mode so it can be programmed over USB. The first programming pass flashes the firmware *and* moves the boot source to the alternate boot pin, which is shared with the USB line; after that, the ground on PB15 is inert.

This also gives the board a recovery path: a fully erased part falls back to the PB15 strap and always re-enters ISP.

Consequences to keep in mind:

- Firmware must never configure PB15 as a push-pull output. Driving it high would short it to ground.
- PB15 is also TCK, so the two-wire debug interface is not usable while the strap is fitted.

#### Improvement for next version: strap through a 4.7 kOhm pull-down

The strap is currently a direct connection to GND. Fitting a resistor between PB15 and GND instead would preserve the ISP behaviour while removing the short if firmware ever drives the pin high.

**Use 4.7 kOhm, 0603, 1% (suggested LCSC C23162)** The boot ROM's internal pull-up sources up to 90 uA (IUP max, datasheet Table 20-2) and VIL is 0.9 V max, so:

| Pull-down | Worst-case V(PB15) | Margin to VIL | Current if driven high |
| --- | --- | --- | --- |
| 10 kOhm | 0.90 V | **none - exactly at the limit** | 0.33 mA |
| **4.7 kOhm** | **0.42 V** | **2.1x** | **0.70 mA** |
| 3.3 kOhm | 0.30 V | 3.0x | 1.0 mA |

Adopting this will also clear the remaining ERC warning (`Pins of type Bidirectional and Power output are connected`), because PB15 leaves the GND net. The cost is placement: the 0603 has to fit near U3 pin 6 without intruding on the antenna keepout, on an already tight board.

See the [WCH CH592 datasheet](https://github.com/openwch/ch592/blob/main/Datasheet/CH592DS1_EN.PDF) for the power, oscillator, RF, USB, and package requirements.

## Fabrication requirements

The antenna geometry and USB connector make the board construction part of the electrical and mechanical design. Use these settings unless a revised PCB has been validated:

| Parameter | Requirement |
| --- | --- |
| Layer count | 2 copper layers |
| Finished board size | 16.35 mm x 9.90 mm |
| Finished thickness | 0.8 mm nominal (35 um + 0.73 mm core + 35 um) |
| Material | FR-4 |
| Copper | 1 oz / 35 um outer copper |
| Surface finish | ENIG recommended; hard gold on the USB contacts is preferred for high insertion-cycle production |
| Minimum finished drill | 0.20 mm |
| Minimum track/space used | 0.15 mm track, 0.127 mm design-rule clearance minimum |
| Soldermask | Both sides; no soldermask over the USB contacts or antenna conductor |

Additional requirements and checks:

1. Preserve the antenna copper, ground keepout, board thickness, and via fence exactly. The antenna is designed for 0.8 mm FR-4; a different thickness detunes it. Do not allow the fabricator to add serial numbers, tooling features, or anything else in the antenna region.
2. Keep the board outline and USB contact geometry at 1:1 scale. If an edge bevel is requested, confirm that the fabricator supports it on a 0.8 mm board without shortening or damaging the USB contacts.
3. Verify the board USB contact fits in several Type-A receptacles before a production run.
4. The enclosure and carrier must not place metal or conductive material over the antenna region.

## Suggested BOM

The listed LCSC/JLCPCB numbers are suggested assembly parts, not sole-source requirements. Substitutes should match the value, package, dielectric/tolerance, voltage rating, and crystal parameters. There is no C3, this will be fixed in the next spin of the hardware.

| References | Qty | Value / part | Package | Suggested LCSC part | Notes |
| --- | ---: | --- | --- | --- | --- |
| U3 | 1 | WCH CH592D | QFN-20, 3 mm x 3 mm, exposed pad | C41417128 | Confirm pin-1 orientation and exposed-pad soldering |
| U2 | 1 | XC6206P332MR-G, 3.3 V LDO | SOT-23-3 | C5446 | |
| Y1 | 1 | 32 MHz crystal | 3225, 4 pad | C7294588 | No external load capacitors fitted |
| C1, C2, C4 | 3 | 1 uF | 0603 | C15849 | X5R or better, 10 V minimum |
| C5 | 1 | 470 nF | 0603 | C47339 | Alternate: C1623 |

AE2 (PCB antenna) and J1 (PCB USB connector) are fabricated as PCB copper and are not assembly parts.

## Source attribution

- The 0.8 mm FR-4 antenna is based on the WCH reference antenna distributed in the [openwch schematic/PCB library](https://github.com/openwch/schpcb_lib).
- The PCB USB connector footprint is derived from the [USB armory](https://github.com/usbarmory/usbarmory) hardware design.

## License

This hardware design is licensed under the CERN Open Hardware Licence Version 2 - Weakly Reciprocal.
