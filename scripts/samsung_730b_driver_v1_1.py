#!/usr/bin/env python3
"""
Samsung 730B 지문인식 드라이버 (v1.1)

- 리눅스용, libfprint 포팅 전에 파이썬으로 구현한거임
- 초기화/캡처 시퀀스는 Windows 트래픽 기반으로 재현했음
- 손가락 감지는 아직 USB 레벨 프로토콜 미해결이라,
  현재 버전은 '수동(엔터 입력)' 방식으로 처리함 (TODO)
- 지문 레이아웃:
  - 캡처 데이터 전체 길이 21506 bytes
  - 유효 지문 영역: offset 182부터 112x96 (세로줄/가로줄 없는 오프셋과 해상도)
  - 왼쪽으로 90도 회전해야 내가보는 방향과 맞음

Copylight 2025 lignah
"""

import usb.core
import usb.util
import time
import argparse
from datetime import datetime
from enum import Enum, auto

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False


class SensorState(Enum):
    DISCONNECTED = auto()
    INITIALIZING = auto()
    IDLE = auto()
    AWAIT_FINGER = auto()
    CAPTURING = auto()
    ERROR = auto()


class Samsung730BError(Exception):
    """드라이버 에러"""


class CaptureError(Samsung730BError):
    """캡처 실패"""


class Samsung730B:
    VID = 0x04e8
    PID = 0x730b

    BULK_IN_EP = 0x82
    BULK_OUT_EP = 0x01
    BULK_PACKET_SIZE = 256

    # 이미지 레이아웃 (다른 해상도로 하면 지문같지 않음)
    IMAGE_WIDTH = 112
    IMAGE_HEIGHT = 96
    IMAGE_SIZE = IMAGE_WIDTH * IMAGE_HEIGHT

    # 유효 지문 데이터 시작 오프셋 (offset 0부터 일일이 다 체크했음 -> 세로줄/가로줄 없음)
    BEST_OFFSET = 182

    CAPTURE_CHUNK_SIZE = 256
    CAPTURE_NUM_CHUNKS = 85
    CAPTURE_START_INDEX = 0x032A

    def __init__(self, debug=False):
        self.dev = None
        self.ep_out = None
        self.ep_in = None
        self.state = SensorState.DISCONNECTED
        self.debug = debug

    # ---------- 내부 헬퍼 ----------

    def _log(self, msg):
        if self.debug:
            print(f"[DEBUG] {msg}")

    def _ensure_open(self):
        if not self.dev:
            raise Samsung730BError("장치가 열려있지 않음")

    # ---------- 장치 제어 ----------

    def open(self):
        """장치 열고 센서 초기화함"""
        self.state = SensorState.INITIALIZING

        self.dev = usb.core.find(idVendor=self.VID, idProduct=self.PID)
        if self.dev is None:
            self.state = SensorState.ERROR
            raise Samsung730BError("Samsung 730B 장치를 찾을 수 없음")

        if self.dev.is_kernel_driver_active(0):
            self._log("커널 드라이버 분리 시도")
            self.dev.detach_kernel_driver(0)

        self.dev.set_configuration()
        cfg = self.dev.get_active_configuration()
        intf = cfg[(0, 0)]

        self.ep_out = usb.util.find_descriptor(
            intf, bEndpointAddress=self.BULK_OUT_EP
        )
        self.ep_in = usb.util.find_descriptor(
            intf, bEndpointAddress=self.BULK_IN_EP
        )

        if not self.ep_out or not self.ep_in:
            self.state = SensorState.ERROR
            raise Samsung730BError("엔드포인트 못찾음")

        self._init_sensor()
        self.state = SensorState.IDLE
        return True

    def close(self):
        """장치 닫기"""
        if self.dev:
            try:
                usb.util.dispose_resources(self.dev)
            except Exception:
                pass
        self.dev = None
        self.ep_in = None
        self.ep_out = None
        self.state = SensorState.DISCONNECTED

    # ---------- USB 통신 ----------

    def _ctrl_write(self, request, value, index, data, timeout=500):
        """CONTROL OUT (vendor-specific)"""
        self._ensure_open()
        bmRequestType = 0x40  # Host→Device, Vendor, Device

        self._log(
            f"ctrl_write: req=0x{request:02x}, wValue=0x{value:04x}, "
            f"wIndex=0x{index:04x}, len={0 if data is None else len(data)}"
        )

        return self.dev.ctrl_transfer(
            bmRequestType, request, value, index,
            data, timeout=timeout
        )

    def _bulk_out(self, data, timeout=500):
        self._ensure_open()
        self._log(f"bulk OUT ({len(data)} bytes): {data[:16].hex(' ')} ...")
        return self.ep_out.write(data, timeout=timeout)

    def _bulk_in(self, size=None, timeout=500):
        self._ensure_open()
        if size is None:
            size = self.BULK_PACKET_SIZE
        try:
            data = bytes(self.ep_in.read(size, timeout=timeout))
            self._log(f"bulk IN ({len(data)} bytes): {data[:16].hex(' ')} ...")
            return data
        except usb.core.USBTimeoutError:
            self._log("bulk IN 타임아웃")
            return None

    # ---------- 센서 초기화 ----------

    def _init_sensor(self):
        """센서 초기화 루틴 : Windows 초기화시퀀스 그대로 재현함"""

        # 1) control 0xC3 초기 설정
        ctrl_data = bytes([
            0x80, 0x84, 0x1e, 0x00,
            0x08, 0x00, 0x00, 0x01,
            0x01, 0x01, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00
        ])
        self._log("초기 control_transfer (0xC3) 전송")
        self._ctrl_write(0xC3, 0x0000, 0x0000, ctrl_data)

        # 2) 0xA9/0xA8 기반 레지스터 설정 (출처 : new.pcapng Leftover Capture Data)
        init_cmds = [
            bytes([0x4f, 0x80]),
            bytes([0xa9, 0x4f, 0x80]),
            bytes([0xa8, 0xb9, 0x00]),
            bytes([0xa9, 0x60, 0x1b, 0x00]),
            bytes([0xa9, 0x50, 0x21, 0x00]),
            bytes([0xa9, 0x61, 0x00, 0x00]),
            bytes([0xa9, 0x62, 0x00, 0x1a]),
            bytes([0xa9, 0x63, 0x00, 0x1a]),
            bytes([0xa9, 0x64, 0x04, 0x0a]),
            bytes([0xa9, 0x66, 0x0f, 0x80]),
            bytes([0xa9, 0x67, 0x1b, 0x00]),
            bytes([0xa9, 0x68, 0x00, 0x0f]),
            bytes([0xa9, 0x69, 0x00, 0x14]),
            bytes([0xa9, 0x6a, 0x00, 0x19]),
            bytes([0xa9, 0x6c, 0x00, 0x19]),
            bytes([0xa9, 0x40, 0x43, 0x00]),
            bytes([0xa9, 0x41, 0x6f, 0x00]),
            bytes([0xa9, 0x55, 0x20, 0x00]),
            bytes([0xa9, 0x5f, 0x00, 0x00]),
            bytes([0xa9, 0x52, 0x27, 0x00]),
            bytes([0xa9, 0x09, 0x00, 0x00]),
            bytes([0xa9, 0x5d, 0x4d, 0x00]),
            bytes([0xa9, 0x51, 0xa8, 0x25]),
            bytes([0xa9, 0x03, 0x00]),
            bytes([0xa9, 0x38, 0x01, 0x00]),
            bytes([0xa9, 0x3d, 0xff, 0x0f]),
            bytes([0xa9, 0x10, 0x60, 0x00]),
            bytes([0xa9, 0x3b, 0x14, 0x00]),
            bytes([0xa9, 0x2f, 0xf6, 0xff]),
            bytes([0xa9, 0x09, 0x00, 0x00]),
            bytes([0xa9, 0x0c, 0x00]),
            bytes([0xa8, 0x20, 0x00, 0x00]),
            bytes([0xa9, 0x04, 0x00]),
            bytes([0xa8, 0x08, 0x00]),
            bytes([0xa9, 0x09, 0x00, 0x00]),
            bytes([0xa8, 0x3e, 0x00, 0x00]),
            bytes([0xa9, 0x03, 0x00, 0x00]),
            bytes([0xa8, 0x20, 0x00, 0x00]),
            bytes([0xa9, 0x10, 0x00, 0x01]),
            bytes([0xa9, 0x2f, 0xef, 0x00]),
            bytes([0xa9, 0x09, 0x00, 0x00]),
            bytes([0xa9, 0x5d, 0x4d, 0x00]),
            bytes([0xa9, 0x51, 0x3a, 0x25]),
            bytes([0xa9, 0x0c, 0x00]),
            bytes([0xa8, 0x20, 0x00, 0x00]),
            bytes([0xa9, 0x04, 0x00, 0x00]),
            bytes([0xa9, 0x09, 0x00, 0x00]),
        ]

        for idx, cmd in enumerate(init_cmds):
            self._log(f"init_cmd[{idx}]: {cmd.hex(' ')}")
            self._bulk_out(cmd)

    # ---------- 손가락 감지 (25.11.30: 수동) ----------

    def wait_finger_manual(self):
        """
        손가락 감지 - 지금은 윈도우 드라이버의 손가락 감지 방법을 몰라서 일단 수동 모드로만 동작함 (TODO)
        """
        if self.state == SensorState.DISCONNECTED:
            raise Samsung730BError("장치가 열려있지 않음")

        self.state = SensorState.AWAIT_FINGER
        print("[*] 손가락을 센서에 올려놓고 엔터 ...")
        try:
            input()
        except KeyboardInterrupt:
            print("취소")
            self.state = SensorState.IDLE
            return False

        # 손가락 감지된 것으로 일단 진행
        self.state = SensorState.IDLE
        return True

    # ---------- 캡처 ----------

    def capture(self):
        """지문 1프레임 캡처"""
        if self.state not in (SensorState.IDLE, SensorState.AWAIT_FINGER):
            raise Samsung730BError(f"캡처 가능 상태가 아님: {self.state}")

        self.state = SensorState.CAPTURING
        image_data = bytearray()

        for i in range(self.CAPTURE_NUM_CHUNKS):
            offset = i * self.CAPTURE_CHUNK_SIZE
            wIndex = self.CAPTURE_START_INDEX + offset

            # CONTROL 0xCA: 프레임 청크 설정
            self._ctrl_write(0xCA, 0x0003, wIndex & 0xFFFF, None)

            # 첫 청크에서만 capture start 명령 전송
            if i == 0:
                capture_cmd = bytes([0xa8, 0x06, 0x00, 0x00]) + bytes(
                    self.BULK_PACKET_SIZE - 4
                )
                self._log("capture_cmd (a8 06 00 00 ...) 전송")
                self._bulk_out(capture_cmd)

            # 데이터 읽기
            data = self._bulk_in(self.BULK_PACKET_SIZE, timeout=500)
            if not data:
                self._log(f"캡처 중 타임아웃, chunk={i}")
                break

            image_data.extend(data)

            # ACK (전부 0으로 채워진 256 bytes)
            self._bulk_out(bytes(self.BULK_PACKET_SIZE))

        self.state = SensorState.IDLE

        if len(image_data) == 0:
            raise CaptureError("캡처된 데이터가 비어 있음")

        return bytes(image_data)

    # ---------- 이미지 저장 ----------

    def save_image(self, data, prefix=None, raw_only=False, rotate=True):
        """RAW/PNG 저장 (offset + 회전)"""
        if prefix is None:
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            prefix = f"samsung730b_{timestamp}"

        raw_path = f"{prefix}.raw"
        with open(raw_path, "wb") as f:
            f.write(data)
        print(f"[+] RAW 저장됨: {raw_path}")

        if raw_only or not HAS_PIL:
            return raw_path

        needed = self.BEST_OFFSET + self.IMAGE_SIZE
        if len(data) < needed:
            print(f"[-] PNG 생성 불가: data_len={len(data)}, 필요={needed}")
            return raw_path

        try:
            # 1) offset 적용해서 112x96 이미지 생성
            img = Image.frombytes(
                "L",
                (self.IMAGE_WIDTH, self.IMAGE_HEIGHT),
                data[self.BEST_OFFSET : self.BEST_OFFSET + self.IMAGE_SIZE],
            )

            # 2) 센서 실제 방향에 맞게 왼쪽으로 90도 회전
            if rotate:
                img = img.transpose(Image.ROTATE_90)

            png_path = f"{prefix}.png"
            img.save(png_path)
            print(f"[+] PNG 저장됨: {png_path}")
            return png_path

        except Exception as e:
            print(f"[-] PNG 저장 실패: {e}")
            return raw_path


# ---------- CLI ----------

def parse_args():
    p = argparse.ArgumentParser(
        description="Samsung 730B 지문인식 테스트 드라이버"
    )
    p.add_argument(
        "--debug", action="store_true",
        help="디버그 로그 출력"
    )
    p.add_argument(
        "--raw-only", action="store_true",
        help="PNG대신 RAW만 저장"
    )
    p.add_argument(
        "--prefix", type=str, default=None,
        help="저장될 파일이름 prefix지정 (기본: timestamp)"
    )
    return p.parse_args()


def main():
    args = parse_args()

    print("=" * 50)
    print("  Samsung 730B fingerprint driver v1.1")
    print("=" * 50)
    print()

    scanner = Samsung730B(debug=args.debug)

    try:
        # 1) 장치 열기
        scanner.open()
        print("[+] 장치 연결 및 초기화 완료\n")

        # 2) 손가락 올려놓기 (현재: 수동)
        if not scanner.wait_finger_manual():
            print("[-] 손가락 감지 실패 (사용자 취소)")
            return 1

        # 3) 캡처
        data = scanner.capture()
        non_zero = sum(1 for b in data if b != 0)
        print(f"[+] 캡처 완료: {len(data)} bytes, non-zero={non_zero}bytes\n")

        if len(data) < 10000:
            print("[-] 데이터 길이가 너무 짧아서 캡처 실패로 간주")
            return 1

        # 4) 저장
        path = scanner.save_image(
            data,
            prefix=args.prefix,
            raw_only=args.raw_only,
        )

        return 0

    except Samsung730BError as e:
        print(f"\n[드라이버 오류] {e}")
        return 1
    except KeyboardInterrupt:
        print("\n사용자 중단")
        return 1
    except Exception as e:
        print(f"\n[예상치 못한 오류..] {e}")
        return 1
    finally:
        scanner.close()


if __name__ == "__main__":
    raise SystemExit(main())