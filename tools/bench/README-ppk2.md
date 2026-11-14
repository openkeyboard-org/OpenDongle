# ppk2d: Nordic PPK2 bench daemon

Owns the Power Profiler Kit II for the whole bench session and answers current queries over a
unix socket, so gate and ladder scripts get calibrated readings without touching the device.

Why a daemon: the PPK2 opens its series switch whenever the host serial session ends, in ampere
meter mode as well as source mode, and Nordic's app closes it silently on connect. The process
holding the port is therefore what keeps the DUT powered. Every ad-hoc open/read/close (including
the nRF Connect app quitting) is a cold boot of the dongle. Only one client can hold the port.

    python3 -m venv .ppkvenv && .ppkvenv/bin/pip install -r requirements-ppk2.txt
    PPK=.ppkvenv/bin/python3          # every invocation needs the venv interpreter
    nohup $PPK ppk2d.py serve --log logs &        # start once per bench session
    $PPK ppk2d.py status
    $PPK ppk2d.py mark pre; ...; $PPK ppk2d.py mark post
    $PPK ppk2d.py stats --since pre --until post   # exact percentiles inside the ring
    $PPK ppk2d.py stats --last 60
    $PPK ppk2d.py power off; sleep 1; $PPK ppk2d.py power on   # cold-boot the DUT
    $PPK ppk2d.py raw --seconds 2 --out trace.csv              # 100 kS/s slice for waveforms
    $PPK ppk2d.py quit                                         # DUT loses power

Notes: reads the full calibration metadata itself (the library's own loop drops chunks on macOS
and silently falls back to default shunt values); refuses to run unless the device reports ampere
meter mode, and never sends REGULATOR_SET in that mode. `serve --allow-source` is an explicit
opt-in that lifts the ampere-only check; use it ONLY when the PPK2 is wired as a source meter and
its configured VDD is safe for the DUT, since in source mode the PPK2 powers the DUT itself. The
daemon still never programs the regulator. Also: macOS lists both PPK2 CDC ports, the
lower interface is the one that answers; per-second min/mean/max go to the CSV log.
