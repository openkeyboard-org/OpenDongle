# OpenDongle CH570

OpenDongle CH570 is a compact USB Type-A 2.4 GHz receiver built around the WCH CH570Q RISC-V wireless MCU. The PCB is 16.4 mm x 10.0 mm and uses a PCB-edge USB connector and a WCH-derived 2.4 GHz PCB antenna.

## Design files

| File or directory | Purpose |
| --- | --- |
| `OpenDongle-CH570.kicad_pro` | KiCad 10 project and design-rule settings |
| `OpenDongle-CH570.kicad_sch` | Schematic |
| `OpenDongle-CH570.kicad_pcb` | PCB layout |
| `lib/OpenDongle.kicad_sym` | Project-owned CH570Q and four-pin PCB USB symbols |
| `lib/OpenDongle.pretty/` | Project-owned USB edge connector and WCH antenna footprints |
| `sym-lib-table`, `fp-lib-table` | Portable project-local library configuration |

Open the project with KiCad 10 or later. The library tables use `${KIPRJMOD}` paths and do not depend on a developer's global KiCad configuration.

## Electrical design notes

- USB VBUS supplies the CH570Q V5 pin directly.
- R1 is the 1.5 kOhm connection required by WCH between V5 and the internally regulated VDD33/VIO33 rail for single-5 V operation.
- Both V5 and VDD33/VIO33 have 2.2 uF bulk and 100 nF local bypass capacitors.
- Y1 is a 32 MHz, 8 pF-load crystal. The CH570 provides programmable internal crystal load capacitance, so external load capacitors are not fitted.
- The RF output connects directly to the WCH-derived PCB antenna, as recommended by the CH570 datasheet.
- AE2 and J1 represent copper artwork fabricated as part of the PCB. They are intentionally excluded from the assembly BOM and position file.

See the [WCH CH572/CH570 datasheet](https://www.wch-ic.com/downloads/CH572DS1_PDF.html) for the power, oscillator, RF, USB, and package requirements.

## Fabrication requirements

The antenna geometry and USB connector make the board construction part of the electrical and mechanical design. Use these settings unless a revised PCB has been validated:

| Parameter | Requirement |
| --- | --- |
| Layer count | 2 copper layers |
| Finished board size | 16.4 mm x 10.0 mm |
| Finished thickness | 0.8 mm nominal |
| Material | FR-4 |
| Copper | 1 oz / 35 um outer copper |
| Surface finish | ENIG recommended; hard gold on the USB contacts is preferred for high insertion-cycle production |
| Minimum finished drill | 0.20 mm |
| Minimum track/space used | 0.15 mm track, 0.127 mm design-rule clearance minimum |
| Soldermask | Both sides; no soldermask over the USB contacts or antenna conductor openings defined by the design |

Additional requirements and checks:

1. Preserve the antenna copper, ground keepout, board thickness, and via fence exactly. Do not allow the fabricator to add copper, serial numbers, tooling features, or panel tabs in the antenna region.
2. Keep the board outline and USB contact geometry at 1:1 scale. If an edge bevel is requested, confirm that the fabricator supports it on a 0.8 mm board without shortening or damaging the USB contacts.
3. Verify the bare-board USB fit in several Type-A receptacles before a production run. The enclosure or carrier must not place metal or conductive coating over the antenna region.
4. The layout does not claim controlled impedance. Validate RF return loss/range and USB operation on assembled first articles before releasing a production revision.

## Suggested BOM

The listed LCSC/JLCPCB numbers are suggested assembly parts, not sole-source requirements. Substitutes should match the value, package, dielectric/tolerance, voltage rating, and crystal parameters.

| References | Qty | Value / part | Package | Suggested LCSC part | Notes |
| --- | ---: | --- | --- | --- | --- |
| U1 | 1 | WCH CH570Q | DFN-10, 3 mm x 3 mm, exposed pad | C49260680 | Confirm pin-1 orientation and exposed-pad soldering |
| Y1 | 1 | 32 MHz crystal | 2016, 4 pad | C37635332 | 8 pF load, +/-10 ppm tolerance, 40 ohm maximum ESR |
| R1 | 1 | 1.5 kOhm, 1% | 0603 | C22843 | Required V5-to-VDD33 connection |
| C1, C3 | 2 | 2.2 uF | 0603 | C23630 | X5R or better, 10 V minimum; suggested part is 16 V |
| C2, C4 | 2 | 100 nF | 0603 | C14663 | X7R or better, 10 V minimum |

AE2 (PCB antenna) and J1 (USB edge contacts) are fabricated copper features and must not be purchased or placed by the assembler.

## Source attribution

- The 0.8 mm FR-4 antenna is based on the WCH reference antenna distributed in the [openwch schematic/PCB library](https://github.com/openwch/schpcb_lib).
- The PCB USB connector footprint is derived from the [USB armory](https://github.com/usbarmory/usbarmory) hardware design.

## License

This hardware design is licensed under the CERN Open Hardware Licence Version 2 - Weakly Reciprocal. See [`LICENSE`](LICENSE).
