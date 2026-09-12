#!/usr/bin/env python3
"""Regression tests for ppk2d's sample framing. No PPK2 required.

Run directly (`python3 test_ppk2_framing.py`) or under pytest.

Background: ppk2_api.get_samples() loses 4-byte alignment on a short read and
never recovers, which silently corrupted two overnight bench runs. ppk2d frames
samples itself and verifies them against the per-sample counter instead. These
tests pin both halves of that: the library bug we are working around, and the
behaviour of the replacement.
"""
import importlib.util, os, sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("ppk2d", os.path.join(_HERE, "ppk2d.py"))
ppk2d = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ppk2d)
Framer, CNT_MOD = ppk2d.Framer, ppk2d.CNT_MOD


class FakePPK:
    """Stands in for PPK2_API: framing is under test, calibration is not."""
    def __init__(self):
        self.rolling_avg = self.rolling_avg4 = self.prev_range = None
        self.consecutive_range_samples = self.after_spike = 0
    def _handle_raw_data(self, word):
        return word, None


def word(counter, adc=0x1234, rng=0, logic=0):
    return (adc & 0x3FFF) | ((rng & 0x7) << 14) | ((counter % CNT_MOD) << 18) | ((logic & 0xFF) << 24)

def stream(n, start=0, adc=0x1234):
    return b"".join(word(start + i, adc).to_bytes(4, "little") for i in range(n))

def counters(samples):
    return [(w >> 18) & 0x3F for w in samples]


def test_aligned_stream_decodes_every_sample():
    f = Framer(FakePPK())
    got, _, realigned = f.feed(stream(64))
    assert len(got) == 64, len(got)
    assert counters(got) == [i % CNT_MOD for i in range(64)]
    assert not realigned and f.realigns == 0 and f.gaps == 0


def test_partial_words_are_held_not_mangled():
    """The exact shape that breaks the library: bytes arriving 1 at a time."""
    f = Framer(FakePPK())
    data = stream(40)
    got = []
    for i in range(len(data)):
        s, _, _ = f.feed(data[i:i + 1])
        got += s
    assert len(got) == 40, len(got)
    assert counters(got) == [i % CNT_MOD for i in range(40)]
    assert f.realigns == 0


def test_attaching_mid_sample_emits_no_garbage():
    """The daemon attaches to a stream already in flight, so the first bytes may
    land mid-sample. Nothing should be emitted until the boundary is proven."""
    f = Framer(FakePPK())
    got, _, _ = f.feed(stream(64)[2:])          # start 2 bytes into a sample
    assert f.aligned, "should have found the boundary"
    assert got, "should emit once aligned"
    c = counters(got)
    assert all((c[i] - c[i - 1]) % CNT_MOD == 1 for i in range(1, len(c))), c[:8]
    assert f.realigns == 0, "attaching is not a slip"


def test_byte_slip_is_detected_and_corrected():
    f = Framer(FakePPK())
    f.feed(stream(32))
    # Two bytes vanish mid-stream: every following word is misframed.
    slipped = stream(64, start=32)[2:]
    got, _, realigned = f.feed(slipped)
    assert realigned, "a 2-byte slip must be detected"
    assert f.realigns == 1
    assert got, "realignment must still yield samples"
    c = counters(got)
    assert all((c[i] - c[i - 1]) % CNT_MOD == 1 for i in range(1, len(c))), c[:8]


def test_good_prefix_survives_a_mid_batch_slip():
    """A slip usually starts part way through a batch. The words before it are
    good and must be emitted as-is; re-framing the whole batch would turn them
    into garbage, which is how a bad sample reached the magnitude guard on the
    first bench run of this decoder."""
    f = Framer(FakePPK())
    f.feed(stream(32))
    batch = stream(16, start=32) + stream(48, start=48)[2:]   # slip after 16 good samples
    got, _, _ = f.feed(batch)
    assert len(got) == 16, f"expected the 16 good samples, got {len(got)}"
    assert counters(got) == [(32 + i) % CNT_MOD for i in range(16)]
    rest, _, realigned = f.feed(b"")
    assert realigned and f.realigns == 1
    c = counters(rest)
    assert all((c[i] - c[i - 1]) % CNT_MOD == 1 for i in range(1, len(c))), c[:8]


def test_whole_sample_loss_is_a_gap_not_a_slip():
    """Dropping a multiple of 4 bytes breaks counter continuity but not framing.
    That is an overrun, not corruption, and must not trigger a re-alignment."""
    f = Framer(FakePPK())
    f.feed(stream(32))
    got, _, realigned = f.feed(stream(32, start=40))   # 8 samples missing
    assert not realigned and f.realigns == 0
    assert f.gaps == 1
    assert len(got) == 32, len(got)


def test_gap_then_slip_in_one_batch_does_not_leak_garbage():
    """A gap and a slip can land in the same batch. _first_break stops at the
    first one, so without a re-scan the words after the slip would be decoded
    and would reach the spike filter without any realignment."""
    f = Framer(FakePPK())
    f.feed(stream(32))
    batch = stream(16, start=48) + stream(32, start=64)[2:]   # gap, then a slip
    got, _, realigned = f.feed(batch)
    assert not realigned, "the leading break is a gap, not a slip"
    assert f.gaps == 1
    assert len(got) == 16, f"must stop at the slip, got {len(got)}"
    c = counters(got)
    assert all((c[i] - c[i - 1]) % CNT_MOD == 1 for i in range(1, len(c))), c
    rest, _, realigned2 = f.feed(b"")
    assert realigned2 and f.realigns == 1, "the slip is caught on the next pass"


def test_filter_state_is_reset_on_realignment():
    ppk = FakePPK()
    ppk.rolling_avg, ppk.prev_range, ppk.after_spike = 1.0, "3", 2
    f = Framer(ppk)
    f.feed(stream(32))
    f.feed(stream(64, start=32)[2:])
    assert f.realigns == 1
    assert ppk.rolling_avg is None and ppk.prev_range is None and ppk.after_spike == 0


def test_library_bug_is_real():
    """Why ppk2d does not call ppk2_api.get_samples(): a 1-byte remainder
    followed by a 1-byte read leaves remainder['len'] negative, after which the
    same aligned bytes decode differently and never recover."""
    try:
        # Every private name this test depends on is resolved HERE, inside the
        # handler, including the bound get_samples. If any of them moves, the
        # test fails with a clear message instead of letting an AttributeError
        # escape and abort the run before the tests that sort after it.
        from ppk2_api.ppk2_api import PPK2_API
        p = object.__new__(PPK2_API)
        p.remainder = {"sequence": b"", "len": 0}
        p._digital_to_analog = lambda b: int.from_bytes(b, "little", signed=False)
        p._handle_raw_data = lambda v: (v, None)
        get_samples = p.get_samples
    except (ImportError, AttributeError) as e:
        raise AssertionError(f"cannot probe ppk2_api internals: {e!r}") from e

    # Assertions stay outside the handler: an unrelated failure in here is a
    # real fault and should keep its own traceback.
    good = b"".join(i.to_bytes(4, "little") for i in range(6))
    assert get_samples(good)[0] == [0, 1, 2, 3, 4, 5]
    p.remainder = {"sequence": b"", "len": 0}
    get_samples(bytes(9))        # leaves a 1-byte remainder
    get_samples(bytes(1))        # short read -> remainder len goes negative
    assert p.remainder["len"] < 0, "expected the negative-remainder bug"
    assert get_samples(good)[0] != [0, 1, 2, 3, 4, 5], "expected the stream to be slipped"


if __name__ == "__main__":
    failed = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  ok    {name}")
            except AssertionError as e:
                failed += 1
                print(f"  FAIL  {name}: {e}")
    # Anything other than an AssertionError is a real fault in the test or the
    # code under test, and is deliberately left to propagate with its traceback.
    print("all framing tests passed" if not failed else f"{failed} test(s) failed")
    sys.exit(1 if failed else 0)
