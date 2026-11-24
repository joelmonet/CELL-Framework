"""Qualify audio-device capture and monitoring over UDP using virtual ALSA."""

import ctypes
import json
import os
import pathlib
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

from test_network_audio_wave import HOST, listening_endpoint, read_float_pcm, read_receiver_line


def send_input(binary, destination, frames, env, *, capture_only=False, mono=False, sample_rate=48000):
    host, port = destination
    endpoint_options = ("--capture-only",) if capture_only else ("--output", "cell_silence")
    input_id = "cell_mono_capture" if mono else "cell_capture"
    channel_options = ("--mono",) if mono else ()
    result = subprocess.run(
        [binary, "send-input", host, str(port), str(frames),
         "--backend", "alsa", "--input", input_id, "--rate", str(sample_rate),
         *endpoint_options, *channel_options],
        env=env, capture_output=True, text=True, timeout=5,
    )
    assert result.returncode == 0, result.stderr
    assert f"Captured {frames} input frames." in result.stdout, result.stdout
    assert "Missing input: 0 frames. Sender queue dropped: 0 frames." in result.stdout, result.stdout


def relay_input(binary, relay, destination, frames, env, sample_rate):
    # Send a syntactically valid first packet from a different source port.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as unrelated:
        unrelated.sendto(struct.pack("<4sIIIQff", b"CNA1", sample_rate, 2, 1, 0, 0.75, 0.75),
                         destination)
    send_input(binary, relay.getsockname(), frames, env, sample_rate=sample_rate)

    next_sample = 0
    while next_sample < frames:
        datagram, _ = relay.recvfrom(1201)
        _, _, _, count, first = struct.unpack_from("<4sIIIQ", datagram)
        assert first == next_sample and count > 0
        relay.sendto(datagram, destination)
        next_sample += count
    assert next_sample == frames


def stream(binary, receive_args, sender_frames, env, relay=None, *, capture_only=False, mono=False, sample_rate=48000):
    with subprocess.Popen(
        [binary, *receive_args],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    ) as receiver:
        try:
            destination = listening_endpoint(receiver)
            if relay is None:
                send_input(binary, destination, sender_frames, env, capture_only=capture_only,
                           mono=mono, sample_rate=sample_rate)
            else:
                relay_input(binary, relay, destination, sender_frames, env, sample_rate)
            report, _ = receiver.communicate(timeout=5)
            assert receiver.returncode == 0, report
            assert "Concealed 0 source frames, unfilled 0 output frames." in report, report
            return report
        finally:
            if receiver.poll() is None:
                receiver.kill()


def read_stats(line):
    assert line.startswith("Stats "), line
    return json.loads(line.removeprefix("Stats "))


def wait_for_status(process, expected, snapshots):
    for _ in range(40):
        line = read_receiver_line(process)
        if line == expected + "\n":
            return
        snapshots.append(read_stats(line))
    raise AssertionError(f"Monitor did not report {expected!r}")


def check_continuous_input(binary, env):
    def read_packet(sink):
        packet, _ = sink.recvfrom(1201)
        magic, rate, channels, count, first = struct.unpack_from("<4sIIIQ", packet)
        assert (magic, rate, channels) == (b"CNA1", 48000, 2) and count > 0
        return first, count

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
        sink.bind((HOST, 0))
        sink.settimeout(5)
        port = sink.getsockname()[1]
        with subprocess.Popen(
            [binary, "send-input", HOST, str(port), "continuous", "--stats-ms", "100",
             "--backend", "alsa", "--input", "cell_capture", "--output", "cell_silence"],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        ) as sender:
            try:
                first, count = read_packet(sink)
                assert first == 0
                snapshot = read_stats(read_receiver_line(sender))
                assert snapshot["mode"] == "send-input", snapshot
                assert snapshot["sample_rate_hz"] == 48000, snapshot
                assert snapshot["device_running"] is True and snapshot["device_interrupted"] is False, snapshot
                assert snapshot["device_callbacks"] > 0, snapshot
                assert snapshot["sent_packets"] > 0 and snapshot["device_frames"] >= count, snapshot
                sink.close()
                # Leave the destination unbound while the virtual input keeps sending.
                time.sleep(0.10)
                assert sender.poll() is None, "Continuous sender exited while its peer was down"
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as resumed:
                    resumed.bind((HOST, port))
                    resumed.settimeout(5)
                    after, count_after = read_packet(resumed)
                    assert after > count
                    sender.send_signal(signal.SIGINT)
                    report, _ = sender.communicate(timeout=5)
                assert sender.returncode == 0, report
                match = re.search(r"Captured (\d+) input frames", report)
                assert match and int(match[1]) >= after + count_after, report
                dropped = re.search(r"UDP send drops: (\d+) packets", report)
                assert dropped and int(dropped[1]) > 0, report
            finally:
                if sender.poll() is None:
                    sender.kill()


def send_test_packets(sender, destination, first_sample, sample_rate, *, packets=40):
    payload = struct.pack("<ff", 0.25, -0.25) * 120
    for index in range(packets):
        header = struct.pack("<4sIIIQ", b"CNA1", sample_rate, 2, 120, first_sample + index * 120)
        sender.sendto(header + payload, destination)


def check_continuous_monitor(binary, env, output_directory):
    sample_rate = 44100
    with subprocess.Popen(
        [binary, "monitor", HOST, "0", "continuous", "20", "--stats-ms", "100", "--backend", "alsa",
         "--output", "cell_reconnect", "--rate", str(sample_rate)],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    ) as monitor:
        try:
            snapshots = []
            destination = listening_endpoint(monitor)
            # The existing ALSA output stays usable, but reopening its path fails.
            output_directory.rename(output_directory.with_name("unlinked-output"))
            initial = read_stats(read_receiver_line(monitor))
            assert initial["session"] == 1 and initial["transport_state"] == "waiting", initial
            assert initial["peer"] is None and initial["last_accepted_packet_age_ms"] is None, initial
            assert initial["device_running"] is False and initial["device_callbacks"] == 0, initial
            assert initial["sample_rate_hz"] == sample_rate and initial["arrival_budget_ms"] == 20, initial
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as first, \
                 socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as second:
                first.bind((HOST, 0))
                second.bind((HOST, 0))
                send_test_packets(first, destination, 480000, sample_rate)
                wait_for_status(monitor, "Sender connected.", snapshots)
                send_test_packets(second, destination, 480000 + 4800, sample_rate, packets=1)
                wait_for_status(monitor, "Sender idle. Waiting for a new stream.", snapshots)
                first_snapshot = snapshots[-1]
                assert first_snapshot["session"] == 1 and first_snapshot["accepted_packets"] == 40, first_snapshot
                assert first_snapshot["state"] == "playing" and first_snapshot["device_frames"] > 0, first_snapshot
                assert first_snapshot["device_running"] is True and first_snapshot["device_interrupted"] is False
                assert first_snapshot["device_callbacks"] > 0 and first_snapshot["concealed_source_frames"] > 0
                assert first_snapshot["transport_state"] == "stalled", first_snapshot
                assert first_snapshot["last_accepted_packet_age_ms"] >= 250, first_snapshot
                assert first_snapshot["peer"] == f"{HOST}:{first.getsockname()[1]}", first_snapshot
                assert first_snapshot["unexpected_peer_packets"] == 1, first_snapshot
                assert any(stats["transport_state"] == "active" and stats["last_accepted_packet_age_ms"] < 250
                           for stats in snapshots), snapshots
                assert listening_endpoint(monitor) == destination
                waiting = read_stats(read_receiver_line(monitor))
                assert waiting["session"] == 2 and waiting["state"] == "waiting", waiting
                assert waiting["accepted_packets"] == 0 and waiting["device_frames"] >= first_snapshot["device_frames"]
                assert waiting["transport_state"] == "waiting" and waiting["device_running"] is False, waiting
                assert waiting["peer"] is None and waiting["last_accepted_packet_age_ms"] is None, waiting
                send_test_packets(second, destination, 0, sample_rate)
                wait_for_status(monitor, "Sender connected.", snapshots)
                for _ in range(10):
                    second_snapshot = read_stats(read_receiver_line(monitor))
                    assert second_snapshot["mode"] == "monitor" and second_snapshot["session"] == 2
                    queued = second_snapshot["queued_playback_frames"]
                    assert queued >= 0 and second_snapshot["queued_playback_ms"] == queued * 1000 // sample_rate
                    if (second_snapshot["state"] == "playing" and
                            second_snapshot["device_frames"] > waiting["device_frames"]):
                        break
                else:
                    raise AssertionError("No playing snapshot for the reconnected session")
                assert second_snapshot["accepted_packets"] == 40, second_snapshot
                assert second_snapshot["peer"] == f"{HOST}:{second.getsockname()[1]}", second_snapshot
            monitor.send_signal(signal.SIGINT)
            report, _ = monitor.communicate(timeout=5)
            assert monitor.returncode == 0, report
            match = re.search(r"Monitored (\d+) frames from (\d+) packets", report)
            assert match and int(match[1]) > 0 and int(match[2]) == 80, report
        finally:
            if monitor.poll() is None:
                monitor.kill()


def check_devices(binary, root):
    input_pcm = root / "input.raw"
    mono_pcm = root / "mono-input.raw"
    output_pcm = root / "output.raw"
    reconnect_output = root / "reconnect-output"
    reconnect_output.mkdir()
    reconnect_sink = reconnect_output / "sink"
    reconnect_sink.symlink_to("/dev/null")
    config = root / "alsa.conf"
    ramp = tuple((1024 + frame) / 16384 for frame in range(16384))
    samples = tuple(sample for value in ramp for sample in (value, -value))
    input_pcm.write_bytes(struct.pack(f"={len(samples)}f", *samples))
    mono_pcm.write_bytes(struct.pack(f"={len(ramp)}f", *ramp))

    config.write_text(f"""
namehint.showall on
pcm.cell_capture {{
    type file
    slave.pcm {{ type null }}
    file "/dev/null"
    infile {json.dumps(str(input_pcm), ensure_ascii=False)}
    format "raw"
    hint {{ show on description "Cell test input" }}
}}
pcm.cell_mono_capture {{
    type file
    slave.pcm {{ type null }}
    file "/dev/null"
    infile {json.dumps(str(mono_pcm), ensure_ascii=False)}
    format "raw"
    hint {{ show on description "Cell test mono input" }}
}}
pcm.cell_monitor {{
    type file
    slave.pcm {{ type null }}
    file {json.dumps(str(output_pcm), ensure_ascii=False)}
    format "raw"
    hint {{ show on description "Cell test output" }}
}}
pcm.cell_silence {{
    type null
    hint {{ show on description "Cell test silent output" }}
}}
pcm.cell_reconnect {{
    type file
    slave.pcm {{ type null }}
    file {json.dumps(str(reconnect_sink), ensure_ascii=False)}
    format "raw"
    hint {{ show on description "Cell test reconnect output" }}
}}
pcm.!default {{ type null }}
""", encoding="utf-8")
    env = dict(os.environ, ALSA_CONFIG_PATH=str(config))

    listing = subprocess.run(
        [binary, "devices", "alsa"], env=env, check=True,
        capture_output=True, text=True, timeout=5,
    ).stdout
    assert "Input\tcell_capture\tCell test input" in listing, listing
    assert "Output\tcell_monitor\tCell test output" in listing, listing

    wav = root / "captured.wav"
    capture_frames = 4801
    stream(binary, ["receive", HOST, "0", str(capture_frames), wav], capture_frames, env)
    assert read_float_pcm(wav, capture_frames) == samples[:capture_frames * 2]

    capture_config = root / "alsa-capture-only.conf"
    capture_config.write_text(config.read_text(encoding="utf-8").replace(
        "pcm.!default { type null }",
        'pcm.!default { type asym capture.pcm "cell_capture" playback.pcm "missing_output" }',
    ), encoding="utf-8")
    capture_env = dict(env, ALSA_CONFIG_PATH=str(capture_config))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as unused_sink:
        unused_sink.bind((HOST, 0))
        duplex = subprocess.run(
            [binary, "send-input", HOST, str(unused_sink.getsockname()[1]), str(capture_frames),
             "--backend", "alsa", "--input", "cell_capture"],
            env=capture_env, capture_output=True, text=True, timeout=5,
        )
    assert duplex.returncode != 0 and "Cannot open audio device" in duplex.stderr, duplex.stderr
    capture_wav = root / "capture-only.wav"
    stream(binary, ["receive", HOST, "0", str(capture_frames), capture_wav],
           capture_frames, capture_env, capture_only=True)
    assert read_float_pcm(capture_wav, capture_frames) == samples[:capture_frames * 2]

    mono_wav = root / "mono.wav"
    stream(binary, ["receive", HOST, "0", str(capture_frames), mono_wav, "--rate", "44100"],
           capture_frames, capture_env, capture_only=True, mono=True, sample_rate=44100)
    assert read_float_pcm(mono_wav, capture_frames, sample_rate=44100) == tuple(
        sample for value in ramp[:capture_frames] for sample in (value, value)
    )


    monitor_frames = 3000
    # The virtual sink runs as fast as callbacks can fill it. Preload enough
    # input for the whole monitor run, plus interpolation lookahead.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as relay:
        relay.bind((HOST, 0))
        relay.settimeout(3)
        report = stream(
            binary, ["monitor", HOST, "0", str(monitor_frames), "50", "--backend", "alsa",
                     "--peer", HOST, str(relay.getsockname()[1]), "--output", "cell_monitor", "--rate", "44100"],
            6001, env, relay, sample_rate=44100,
        )
    assert "Unexpected peer packets: 1." in report, report
    assert f"Monitored {monitor_frames} frames" in report, report
    output = struct.iter_unpack("=ff", output_pcm.read_bytes())
    active = [(left, right) for left, right in output if left != 0 or right != 0]
    assert len(active) == monitor_frames, f"Expected {monitor_frames} active frames, got {len(active)}"
    for index, (left, right) in enumerate(active):
        assert abs(left - ramp[index]) <= 4 / 16384 and left == -right, (index, left, right)
    check_continuous_input(binary, env)
    check_continuous_monitor(binary, env, reconnect_output)
    print("Virtual ALSA stereo and mono capture, peer-pinned playback, reconnect on one output, and shutdown passed.")


def main():
    try:
        ctypes.CDLL("libasound.so.2")
    except OSError:
        print("Skipping virtual audio devices: libasound.so.2 is unavailable.")
        return 77
    with tempfile.TemporaryDirectory(prefix="cell-network-devices-") as directory:
        check_devices(sys.argv[1], pathlib.Path(directory))
    return 0


if __name__ == "__main__":
    sys.exit(main())
