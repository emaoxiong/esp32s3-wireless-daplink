import sys
import time
import socket
import struct
from pyocd.probe.debug_probe import DebugProbe
from pyocd.probe import aggregator
from pyocd.__main__ import main as pyocd_main

ELP_HOST = "dap.local"
ELP_PORT = 3240

EL_LINK_IDENTIFIER = 0x8a656c70
EL_COMMAND_HANDSHAKE = 0x00000000
EL_DAP_VERSION = 0x00000001

ID_DAP_INFO = 0x00
ID_DAP_HOST_STATUS = 0x01
ID_DAP_CONNECT = 0x02
ID_DAP_DISCONNECT = 0x03
ID_DAP_TRANSFER_CONFIGURE = 0x04
ID_DAP_TRANSFER = 0x05
ID_DAP_TRANSFER_BLOCK = 0x06
ID_DAP_SWJ_PINS = 0x10
ID_DAP_SWJ_CLOCK = 0x11
ID_DAP_SWJ_SEQUENCE = 0x12
ID_DAP_SWD_CONFIGURE = 0x13


class ElaphureLinkDebugProbe(DebugProbe):
    def __init__(self, host=ELP_HOST, port=ELP_PORT):
        super().__init__()
        self._host = host
        self._port = port
        self._sock = None
        self._is_open = False
        self._reset_asserted = False
        self._protocol = DebugProbe.Protocol.SWD
        self._total_read_bytes = 0
        self._read_start_time = 0.0
        self._caps = {
            DebugProbe.Capability.SWJ_SEQUENCE,
            DebugProbe.Capability.BANKED_DP_REGISTERS,
            DebugProbe.Capability.APv2_ADDRESSES,
        }

    @property
    def description(self):
        return f"ESP32-S3 elaphureLink ({self._host}:{self._port})"

    @property
    def vendor_name(self):
        return "Espressif"

    @property
    def product_name(self):
        return "ESP32-S3 elaphureLink"

    @property
    def supported_wire_protocols(self):
        return [DebugProbe.Protocol.DEFAULT, DebugProbe.Protocol.SWD]

    @property
    def unique_id(self):
        return f"ELP-{self._host}"

    @property
    def wire_protocol(self):
        return self._protocol

    @property
    def is_open(self):
        return self._is_open

    @property
    def capabilities(self):
        return self._caps

    def _recv_exact(self, size):
        buf = bytearray()
        while len(buf) < size:
            chunk = self._sock.recv(size - len(buf))
            if not chunk:
                raise ConnectionError("elaphureLink TCP connection closed")
            buf.extend(chunk)
        return bytes(buf)

    def _dap_cmd(self, payload: bytes) -> bytes:
        self._sock.sendall(payload)
        resp = self._sock.recv(1500)
        if not resp:
            raise ConnectionError("Empty response from ESP32-S3")
        return resp

    def _swd_wakeup_sequence(self):
        self._dap_cmd(bytes([ID_DAP_CONNECT, 1]))
        self._dap_cmd(struct.pack("<BBBI", ID_DAP_SWJ_PINS, 0x83, 0x83, 0))
        time.sleep(0.01)
        self._dap_cmd(bytes([ID_DAP_SWJ_SEQUENCE, 64]) + b"\xff" * 8)
        self._dap_cmd(bytes([ID_DAP_SWJ_SEQUENCE, 16, 0x9E, 0xE7]))
        self._dap_cmd(bytes([ID_DAP_SWJ_SEQUENCE, 64]) + b"\xff" * 8)
        self._dap_cmd(bytes([ID_DAP_SWJ_SEQUENCE, 16, 0x00, 0x00]))

    def open(self):
        if self._is_open:
            return
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock.settimeout(5.0)
        self._sock.connect((self._host, self._port))

        req = struct.pack(">III", EL_LINK_IDENTIFIER, EL_COMMAND_HANDSHAKE, EL_DAP_VERSION)
        self._sock.sendall(req)
        resp = self._recv_exact(12)
        ident, cmd, _ = struct.unpack(">III", resp)
        if ident != EL_LINK_IDENTIFIER or cmd != EL_COMMAND_HANDSHAKE:
            self._sock.close()
            raise RuntimeError(f"elaphureLink handshake failed: {hex(ident)}")

        self._is_open = True
        self._dap_cmd(bytes([ID_DAP_CONNECT, 1]))
        self._dap_cmd(struct.pack("<BBHH", ID_DAP_TRANSFER_CONFIGURE, 2, 100, 0))
        self._dap_cmd(bytes([ID_DAP_SWD_CONFIGURE, 0]))

    def close(self):
        if self._total_read_bytes > 0:
            print()  # 换行收尾进度条
        if self._sock:
            try:
                self._dap_cmd(bytes([ID_DAP_HOST_STATUS, 0, 0]))
                self._dap_cmd(bytes([ID_DAP_DISCONNECT]))
            except Exception:
                pass
            try:
                self._sock.close()
            except Exception:
                pass
            self._sock = None
        self._is_open = False

    def connect(self, protocol=None):
        self._dap_cmd(bytes([ID_DAP_INFO, 0xFF]))
        self._dap_cmd(bytes([ID_DAP_HOST_STATUS, 0, 1]))
        resp = self._dap_cmd(bytes([ID_DAP_CONNECT, 1]))
        if len(resp) < 2 or resp[1] != 1:
            raise RuntimeError(f"DAP_Connect SWD failed: {resp.hex()}")
        self._dap_cmd(struct.pack("<BBHH", ID_DAP_TRANSFER_CONFIGURE, 2, 100, 0))
        self._dap_cmd(bytes([ID_DAP_SWD_CONFIGURE, 0]))
        self._dap_cmd(struct.pack("<BI", ID_DAP_SWJ_CLOCK, 1000000))
        self._swd_wakeup_sequence()
        self._protocol = DebugProbe.Protocol.SWD

    def disconnect(self):
        try:
            self._dap_cmd(bytes([ID_DAP_DISCONNECT]))
        except Exception:
            pass

    def set_clock(self, frequency):
        self._dap_cmd(struct.pack("<BI", ID_DAP_SWJ_CLOCK, int(frequency)))

    def swj_sequence(self, length, bits):
        num_bytes = (length + 7) // 8
        bit_bytes = int(bits).to_bytes(num_bytes, byteorder="little")
        self._dap_cmd(bytes([ID_DAP_SWJ_SEQUENCE, length]) + bit_bytes)

    def assert_reset(self, asserted):
        self._reset_asserted = bool(asserted)
        pin_out = 0x00 if asserted else 0x80
        self._dap_cmd(struct.pack("<BBBI", ID_DAP_SWJ_PINS, pin_out, 0x80, 0))

    def is_reset_asserted(self):
        return self._reset_asserted

    def reset(self):
        self.assert_reset(True)
        time.sleep(0.05)
        self.assert_reset(False)
        time.sleep(0.05)

    def _transfer_with_retry(self, pkt: bytes, expect_len: int, allow_wakeup: bool = False) -> bytes:
        resp = self._dap_cmd(pkt)
        for attempt in range(3):
            if len(resp) >= expect_len and resp[2] == 1:
                return resp
            time.sleep(0.02)
            if allow_wakeup:
                if attempt == 2:
                    self.reset()
                self._swd_wakeup_sequence()
            resp = self._dap_cmd(pkt)
        return resp

    def read_dp(self, addr, now=True):
        req_byte = 0x02 | (addr & 0x0C)
        resp = self._transfer_with_retry(bytes([ID_DAP_TRANSFER, 0, 1, req_byte]), 7, allow_wakeup=True)
        if len(resp) < 7 or resp[2] != 1:
            raise RuntimeError(f"SWD read_dp(0x{addr:X}) ACK error: {resp.hex()}")
        val = struct.unpack("<I", resp[3:7])[0]
        return val if now else (lambda: val)

    def write_dp(self, addr, data):
        req_byte = 0x00 | (addr & 0x0C)
        resp = self._transfer_with_retry(struct.pack("<BBBBI", ID_DAP_TRANSFER, 0, 1, req_byte, data), 3)
        if len(resp) < 3 or resp[2] != 1:
            raise RuntimeError(f"SWD write_dp(0x{addr:X}) ACK error: {resp.hex()}")

    def read_ap(self, addr, now=True):
        req_byte = 0x03 | (addr & 0x0C)
        resp = self._transfer_with_retry(bytes([ID_DAP_TRANSFER, 0, 1, req_byte]), 7)
        if len(resp) < 7 or resp[2] != 1:
            raise RuntimeError(f"SWD read_ap(0x{addr:X}) ACK error: {resp.hex()}")
        val = struct.unpack("<I", resp[3:7])[0]
        return val if now else (lambda: val)

    def write_ap(self, addr, data):
        req_byte = 0x01 | (addr & 0x0C)
        resp = self._transfer_with_retry(struct.pack("<BBBBI", ID_DAP_TRANSFER, 0, 1, req_byte, data), 3)
        if len(resp) < 3 or resp[2] != 1:
            raise RuntimeError(f"SWD write_ap(0x{addr:X}) ACK error: {resp.hex()}")

    def read_ap_multiple(self, addr, count=1, now=True):
        results = []
        req_byte = 0x03 | (addr & 0x0C)
        remain = count
        if self._read_start_time == 0.0:
            self._read_start_time = time.perf_counter()

        while remain > 0:
            chunk = min(remain, 14)
            pkt = struct.pack("<BBHB", ID_DAP_TRANSFER_BLOCK, 0, chunk, req_byte)
            resp = None
            for attempt in range(4):
                resp = self._dap_cmd(pkt)
                if len(resp) >= 4 + chunk * 4 and resp[3] == 1:
                    break
                # 遇错先写 DP ABORT (0x00) 清除粘滞错误位，再重读当前分片
                time.sleep(0.02)
                self._dap_cmd(struct.pack("<BBBBI", ID_DAP_TRANSFER, 0, 1, 0x00, 0x1E))
            if not resp or len(resp) < 4 + chunk * 4 or resp[3] != 1:
                raise RuntimeError(f"SWD read_ap_multiple ACK error: {resp.hex() if resp else 'EMPTY'}")

            words = struct.unpack(f"<{chunk}I", resp[4:4 + chunk * 4])
            results.extend(words)
            remain -= chunk

            # 实时刷新终端读取进度与速率
            self._total_read_bytes += chunk * 4
            if self._total_read_bytes >= 1024 and (self._total_read_bytes % 2048 < chunk * 4):
                kb = self._total_read_bytes / 1024.0
                elapsed = max(time.perf_counter() - self._read_start_time, 0.001)
                speed = kb / elapsed
                sys.stdout.write(f"\r[读取进度] 已读取: {kb:6.1f} KB | 速率: {speed:4.2f} KB/s   ")
                sys.stdout.flush()

        return results if now else (lambda: results)

    def write_ap_multiple(self, addr, values):
        req_byte = 0x01 | (addr & 0x0C)
        vals = list(values)
        idx = 0
        while idx < len(vals):
            chunk_vals = vals[idx:idx + 14]
            chunk = len(chunk_vals)
            pkt = struct.pack(f"<BBHB{chunk}I", ID_DAP_TRANSFER_BLOCK, 0, chunk, req_byte, *chunk_vals)
            resp = None
            for _ in range(4):
                resp = self._dap_cmd(pkt)
                if len(resp) >= 4 and resp[3] == 1:
                    break
                time.sleep(0.02)
                self._dap_cmd(struct.pack("<BBBBI", ID_DAP_TRANSFER, 0, 1, 0x00, 0x1E))
            if not resp or len(resp) < 4 or resp[3] != 1:
                raise RuntimeError(f"SWD write_ap_multiple ACK error: {resp.hex() if resp else 'EMPTY'}")
            idx += chunk


_orig_get_all = aggregator.DebugProbeAggregator.get_all_connected_probes

@staticmethod
def _custom_get_all_connected_probes(*args, **kwargs):
    unique_id = kwargs.get("unique_id", args[0] if args else None)
    probes = list(_orig_get_all(*args, **kwargs))
    probe = ElaphureLinkDebugProbe()
    if unique_id is None or unique_id == probe.unique_id:
        probes.append(probe)
    return probes

aggregator.DebugProbeAggregator.get_all_connected_probes = _custom_get_all_connected_probes

if __name__ == "__main__":
    if "-v" not in sys.argv and "--verbose" not in sys.argv:
        sys.argv.append("-v")
    sys.exit(pyocd_main())

# 写一个hex到芯片 python .\pyocd_elp.py flash -u ELP-dap.local --pack .\Nations.N32G45x_DFP.1.3.0.pack -t n32g452ccl7 -f 2000000 C:\Users\29132\Downloads\Nations.N32G45x_Library.2.6.0\projects\n32g45x_EVAL\examples\USART\Interrupt\MDK-ARM\Objects\Interrupt.hex
# 进入控制端模式 python .\pyocd_elp.py commander -u ELP-dap.local --pack .\Nations.N32G45x_DFP.1.3.0.pack -t n32g452ccl7 -f 2000000