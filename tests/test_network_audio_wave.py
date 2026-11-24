"""Verify the WAV streamer over localhost, with deterministic UDP loss."""
import os
import pathlib
import queue
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave

HOST = "127.0.0.1"
FRAMES = 9601
IMPAIRED_SAMPLE = 1200
RELEASE_SAMPLE = IMPAIRED_SAMPLE + 4800
PACKET_FRAMES = 120


def read_float_pcm(path, frames=FRAMES, *, sample_rate=48000):
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    offset = 12
    format_rate = None
    while offset + 8 <= len(data):
        kind, size = struct.unpack_from("<4sI", data, offset)
        offset += 8
        assert offset + size <= len(data)
        if kind == b"fmt ":
            assert size >= 16
            format_rate = struct.unpack_from("<I", data, offset + 4)[0]
        if kind == b"data":
            assert format_rate == sample_rate
            assert size == frames * 2 * 4
            return struct.unpack_from(f"<{frames * 2}f", data, offset)
        offset += size + size % 2
    raise AssertionError("Capture is missing a data chunk")


def read_receiver_line(receiver):
    # A Windows subprocess pipe cannot be passed to socket.select().
    lines = queue.Queue()
    reader = threading.Thread(target=lambda: lines.put(receiver.stdout.readline()), daemon=True)
    reader.start()
    try:
        line = lines.get(timeout=5)
    except queue.Empty as error:
        raise AssertionError("Receiver did not report its status") from error
    reader.join()
    return line


def listening_endpoint(receiver):
    line = read_receiver_line(receiver)
    match = re.fullmatch(r"Listening on port (\d+)\n", line)
    assert match, line
    return HOST, int(match[1])


def check_idle_timeout(binary, output):
    receiver = subprocess.Popen(
        [binary, "receive", HOST, "0", "480000", output],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        destination = listening_endpoint(receiver)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.sendto(struct.pack("<4sIIIQff", b"CNA1", 48000, 2, 1, 0, 0.5, -0.5),
                          destination)
            log, _ = receiver.communicate(timeout=5)
        assert receiver.returncode != 0 and "UDP stream timed out" in log, log
        assert not output.exists()
        assert not list(output.parent.glob(".cell-network-*"))
    finally:
        if receiver.poll() is None:
            receiver.kill()
        receiver.wait()


def check_queued_packets_after_pause(binary, output):
    frames = 3601
    expected = tuple(sample / 32768 for frame in range(frames) for sample in (frame + 1, -frame - 1))

    def datagram(first, count):
        header = struct.pack("<4sIIIQ", b"CNA1", 48000, 2, count, first)
        samples = (sample for frame in range(first, first + count) for sample in (frame + 1, -frame - 1))
        return header + struct.pack(f"<{count * 2}f", *(sample / 32768 for sample in samples))

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender, subprocess.Popen(
        [binary, "receive", HOST, "0", str(frames), output, "20"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    ) as receiver:
        try:
            destination = listening_endpoint(receiver)
            sender.sendto(datagram(0, 1), destination)
            assert read_receiver_line(receiver) == "Sender connected.\n"
            receiver.send_signal(signal.SIGSTOP)
            _, status = os.waitpid(receiver.pid, os.WUNTRACED)
            assert os.WIFSTOPPED(status)
            try:
                for first in range(1, frames, PACKET_FRAMES):
                    sender.sendto(datagram(first, min(PACKET_FRAMES, frames - first)), destination)
                # The complete burst is queued while the receiver misses a scheduling turn.
                time.sleep(0.15)
            finally:
                receiver.send_signal(signal.SIGCONT)
            report, _ = receiver.communicate(timeout=5)
            assert receiver.returncode == 0, report
            assert "Concealed 0 source frames, unfilled 0 output frames." in report, report
            assert "Late packets: 0." in report, report
            assert read_float_pcm(output, frames) == expected
        finally:
            if receiver.poll() is None:
                receiver.kill()


def exercise(binary, source, output, impairment, budget, concealed, late, restrict_peer=False):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as relay:
        relay.bind((HOST, 0))
        relay.settimeout(3)
        command = [binary, "receive", HOST, "0", str(FRAMES), output]
        if budget is not None:
            command.append(str(budget))
        if restrict_peer:
            command.extend(("--peer", HOST, str(relay.getsockname()[1])))
        receiver = subprocess.Popen(
            command,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        sender = None
        try:
            destination = listening_endpoint(receiver)
            if restrict_peer:
                # A valid packet from a different local port arrives before the allowed stream.
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
                    other.bind((HOST, 0))
                    other.sendto(struct.pack("<4sIIIQff", b"CNA1", 48000, 2, 1, 0, -0.5, -0.5),
                                 destination)
            port = relay.getsockname()[1] if impairment or restrict_peer else destination[1]
            sender = subprocess.Popen([binary, "send", HOST, str(port), source])
            if impairment or restrict_peer:
                affected = 0
                held = None
                for _ in range((FRAMES + PACKET_FRAMES - 1) // PACKET_FRAMES):
                    packet, _ = relay.recvfrom(1201)
                    first_sample = struct.unpack_from("<Q", packet, 16)[0]
                    if impairment and first_sample == IMPAIRED_SAMPLE:
                        affected += 1
                        if impairment == "delay":
                            held = packet
                        continue
                    relay.sendto(packet, destination)
                    if first_sample == RELEASE_SAMPLE and held is not None:
                        relay.sendto(held, destination)
                        held = None
                assert affected == bool(impairment) and held is None

            assert sender.wait(timeout=5) == 0
            log, _ = receiver.communicate(timeout=5)
            assert receiver.returncode == 0, log
            assert f"Concealed {concealed} source frames, unfilled 0 output frames." in log, (
                f"{impairment=} {budget=} {restrict_peer=}\n{log}"
            )
            assert f"Late packets: {late}." in log, log
            assert f"Unexpected peer packets: {int(restrict_peer)}." in log, log
        finally:
            for process in (sender, receiver):
                if process is not None:
                    if process.poll() is None:
                        process.kill()
                    process.wait()


def generate_latency_probe(binary, root):
    probe_file = root / "latency-probe.wav"
    command = [binary, "make-latency-probe", probe_file]
    result = subprocess.run(command, text=True, capture_output=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert "starts at frame 4800 and spans 256 frames." in result.stdout
    with wave.open(str(probe_file), "rb") as reader:
        assert (reader.getframerate(), reader.getnchannels(), reader.getsampwidth()) == (48000, 2, 2)
        assert reader.getnframes() == 120000
        stereo = tuple(struct.iter_unpack("<hh", reader.readframes(120000)))
    assert set(stereo[:4800]) == set(stereo[5056:]) == {(0, 0)}
    assert all(left == right and abs(left) <= 4096 for left, right in stereo)
    burst = [left for left, _ in stereo[4800:5056]]
    assert burst[0] == burst[-1] == 0 and max(burst) > 2000 and min(burst) < -2000

    previous = probe_file.read_bytes()
    result = subprocess.run(command, text=True, capture_output=True, timeout=5)
    assert result.returncode != 0 and probe_file.read_bytes() == previous
    return burst


def check_latency_measurement(binary, root):
    rate = 48000
    reference_frame = 317
    capture_frames = 2000
    probe = generate_latency_probe(binary, root)
    capture = root / "latency.wav"

    def measure(delays, *, inverted=False, reference=reference_frame, extra_reference=None,
                drop_reference=False):
        left = [250] * capture_frames
        recorded_probe = [250 + 2 * sample + index % 7 for index, sample in enumerate(probe)]
        if not drop_reference:
            left[reference_frame:reference_frame + len(probe)] = recorded_probe
        if extra_reference is not None:
            left[extra_reference:extra_reference + len(probe)] = recorded_probe
        right = [600] * capture_frames
        for delay in delays:
            polarity = -1 if inverted else 1
            for index, sample in enumerate(probe):
                # Give the return a DC offset, gain change, and deterministic noise.
                right[reference_frame + delay + index] += polarity * sample // 2 + index % 9 - 4
        with wave.open(str(capture), "wb") as writer:
            writer.setparams((2, 2, rate, 0, "NONE", "not compressed"))
            writer.writeframes(b"".join(struct.pack("<hh", *frame) for frame in zip(left, right)))
        return subprocess.run(
            [binary, "measure-latency", capture, str(reference), "25"],
            text=True, capture_output=True, timeout=5,
        )

    delayed = measure((613,), inverted=True)
    assert delayed.returncode == 0, delayed.stderr
    assert "Delay: 613 frames (12.771 ms)" in delayed.stdout, delayed.stdout
    assert "Correlation: 1.000" in delayed.stdout and "Polarity: inverted" in delayed.stdout

    automatic = measure((613,), inverted=True, reference="auto")
    assert automatic.returncode == 0, automatic.stderr
    assert "Reference frame: 317" in automatic.stdout and "Delay: 613 frames" in automatic.stdout

    repeated = measure((613,), reference="auto", extra_reference=1500)
    assert repeated.returncode != 0 and "Ambiguous reference probe" in repeated.stderr
    no_probe = measure((613,), reference="auto", drop_reference=True)
    assert no_probe.returncode != 0 and "No strong match for the generated reference probe" in no_probe.stderr

    undelayed = measure((0,))
    assert undelayed.returncode == 0 and "Delay: 0 frames" in undelayed.stdout, undelayed.stderr
    ambiguous = measure((613, 1024))
    assert ambiguous.returncode != 0 and "Ambiguous return delay" in ambiguous.stderr
    missing = measure(())
    assert missing.returncode != 0 and "No strong match" in missing.stderr
    short = measure((), reference=900)
    assert short.returncode != 0 and "Capture is too short" in short.stderr


def check_latency_series(binary, root):
    path = root / "two-probes.wav"
    generated = subprocess.run([binary, "make-latency-probe", path, "2"],
                               text=True, capture_output=True, timeout=5)
    assert generated.returncode == 0, generated.stderr
    assert "2 probes spaced 2.5 seconds apart." in generated.stdout
    with wave.open(str(path), "rb") as reader:
        assert reader.getnframes() == 240000
        reader.setpos(4800)
        first = reader.readframes(256)
        reader.setpos(124800)
        assert reader.readframes(256) == first
    probe = [left for left, _ in struct.iter_unpack("<hh", first)]
    references = (317, 120320)
    delays = (480, 577)
    recording = root / "series.wav"
    pcm = bytearray(122000 * 4)
    for reference, delay in zip(references, delays):
        for index, sample in enumerate(probe):
            struct.pack_into("<h", pcm, (reference + index) * 4, sample)
            struct.pack_into("<h", pcm, (reference + delay + index) * 4 + 2, sample)
    with wave.open(str(recording), "wb") as writer:
        writer.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
        writer.writeframes(pcm)

    def measure(*frames):
        return subprocess.run(
            [binary, "measure-latency-series", recording, "25", *map(str, frames)],
            text=True, capture_output=True, timeout=5,
        )

    result = measure(*references)
    assert result.returncode == 0, result.stderr
    assert "Reference 317: 480 frames" in result.stdout
    assert "Reference 120320: 577 frames" in result.stdout
    assert "Median delay: 528.500 frames (11.010 ms)" in result.stdout
    assert "Minimum: 480 frames. Maximum: 577 frames. Spread: 97 frames (2.021 ms)" in result.stdout
    failed = measure(references[0], 10000)
    assert failed.returncode != 0 and "Probe at frame 10000 failed:" in failed.stderr
    assert not failed.stdout
    duplicate = measure(references[0], references[0])
    assert duplicate.returncode != 0 and "Reference frames must be strictly increasing" in duplicate.stderr


def check_stream_rates(binary, root):
    frames = 3601
    pcm = [i % 10001 - 5000 for i in range(frames * 2)]
    for rate in (44100, 96000):
        source = root / f"rate-{rate}.wav"
        output = root / f"captured-{rate}.wav"
        with wave.open(str(source), "wb") as writer:
            writer.setparams((2, 2, rate, 0, "NONE", "not compressed"))
            writer.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other, subprocess.Popen(
            [binary, "receive", HOST, "0", str(frames), output, "150", "--rate", str(rate)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        ) as receiver:
            try:
                host, port = listening_endpoint(receiver)
                other.sendto(struct.pack("<4sIIIQff", b"CNA1", 48000, 2, 1, 0, 0.5, -0.5),
                             (host, port))
                sent = subprocess.run(
                    [binary, "send", host, str(port), source],
                    capture_output=True, text=True, timeout=5,
                )
                assert sent.returncode == 0, sent.stderr
                report, _ = receiver.communicate(timeout=5)
                assert receiver.returncode == 0, report
                assert "Concealed 0 source frames, unfilled 0 output frames." in report, report
                assert read_float_pcm(output, frames, sample_rate=rate) == tuple(x / 32768 for x in pcm)
            finally:
                if receiver.poll() is None:
                    receiver.kill()


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        check_latency_measurement(binary, root)
        check_latency_series(binary, root)
        check_stream_rates(binary, root)
        source = root / "input.wav"
        pcm = [1 + (i * 13 % 15000) for i in range(FRAMES * 2)]
        with wave.open(str(source), "wb") as writer:
            writer.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
            writer.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
        invalid_output = root / "invalid.wav"
        for budget in ("4", "151"):
            result = subprocess.run(
                [binary, "receive", HOST, "0", str(FRAMES), invalid_output, budget],
                capture_output=True, text=True, timeout=2,
            )
            assert result.returncode != 0 and "Numeric argument out of range" in result.stderr
            assert not invalid_output.exists()
        check_idle_timeout(binary, root / "interrupted.wav")
        if hasattr(signal, "SIGSTOP"):
            check_queued_packets_after_pause(binary, root / "queued-burst.wav")
        scenarios = (
            (None, 150, 0, 0),
            ("drop", 150, PACKET_FRAMES, 0),
            ("delay", 60, PACKET_FRAMES, 1),
            ("delay", 150, 0, 0),
            (None, 150, 0, 0, True),
        )
        for index, (impairment, budget, concealed, late, *peer) in enumerate(scenarios):
            output = root / f"captured-{index}.wav"
            exercise(binary, source, output, impairment, budget, concealed, late, *peer)
            expected = tuple(
                0.0 if IMPAIRED_SAMPLE <= index // 2 < IMPAIRED_SAMPLE + concealed
                else value / 32768
                for index, value in enumerate(pcm)
            )
            assert read_float_pcm(output) == expected, f"PCM mismatch for {impairment=} {budget=}"



if __name__ == "__main__":
    main()
