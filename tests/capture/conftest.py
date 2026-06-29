import struct
import subprocess
import pytest
from pathlib import Path

CAPTURE_BIN = Path(__file__).resolve().parents[2] / "host" / "build" / "capture"


@pytest.fixture
def capture(tmp_path):
    """Run the capture binary against a bytes input, return (samples, stderr)."""
    if not CAPTURE_BIN.exists():
        pytest.skip(f"capture binary not found at {CAPTURE_BIN}")

    def run(data: bytes) -> tuple[list[int], str]:
        input_file  = tmp_path / "input.bin"
        output_file = tmp_path / "output.bin"
        input_file.write_bytes(data)

        result = subprocess.run(
            [str(CAPTURE_BIN), str(input_file), str(output_file)],
            capture_output=True, timeout=5,
        )
        assert result.returncode == 0

        raw     = output_file.read_bytes()
        samples = list(struct.unpack(f"<{len(raw) // 2}H", raw))
        return samples, result.stderr.decode()

    return run
