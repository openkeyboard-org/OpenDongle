# OpenDongle Hardware

OpenDongle is a compact USB Type-A 2.4 GHz wireless receiver. This directory contains the KiCad hardware design files, project-local libraries, fabrication notes, and bill-of-materials information for each implementation.

## Implementations

| Implementation | MCU | Board size | Power | Status |
| --- | --- | --- | --- | --- |
| [CH570](CH570/) | WCH CH570Q (DFN-10) | 16.4 mm x 10.0 mm | VBUS direct to V5, internal regulator | Available |
| [CH592](CH592/) | WCH CH592D (QFN-20) | 16.35 mm x 9.90 mm | VBUS via XC6206 3.3 V LDO | Available |

Both implementations share the same concept: a PCB-edge USB Type-A connector, a WCH-derived 2.4 GHz PCB antenna on 0.8 mm FR-4, and a two-layer board small enough to sit inside a standard nano-receiver shell. They differ in the MCU and therefore in the power and decoupling arrangement.

See each implementation's README for detailed electrical, fabrication, assembly, and licensing information.

## Shared fabrication constraints

Both boards use the same stackup and design rules, because the antenna is only tuned for this construction:

| Parameter | Requirement |
| --- | --- |
| Layer count | 2 copper layers |
| Finished thickness | 0.8 mm nominal (35 um + 0.73 mm core + 35 um) |
| Material | FR-4 |
| Copper | 1 oz / 35 um outer copper |
| Minimum finished drill | 0.20 mm |
| Minimum track / clearance used | 0.15 mm track, 0.127 mm clearance |

## Verifying a design

Both projects open with KiCad 10 or later and resolve every symbol and footprint from their own directory via `${KIPRJMOD}`, so nothing needs to be added to your global library tables.

You can check either board without opening the GUI:

```sh
kicad-cli sch erc --severity-all hardware/CH570/OpenDongle-CH570.kicad_sch
kicad-cli pcb drc --severity-all --schematic-parity hardware/CH570/OpenDongle-CH570.kicad_pcb
```

CH570 is clean. CH592 reports a single expected ERC warning, explained in its README.

## Inspiration

This hardware design was inspired by the [bplug-ch project](https://oshwhub.com/lightandelectricity/bplug-ch).

## License

The hardware designs are licensed under the CERN Open Hardware Licence Version 2 - Weakly Reciprocal. See [LICENSE](LICENSE).
