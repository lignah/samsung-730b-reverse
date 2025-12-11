최종 업데이트 날짜: 25.12.12

# Samsung 730B 지문 센서 Vendor 프로토콜 정리

Samsung 730B 지문 센서(VID 0x04E8, PID 0x730B)에 대한 USB 프로토콜 문서

    Windows USBPcap + Wireshark로 캡처한 드라이버 트래픽
    리눅스 usbmon + libusb/pyusb 기반 리버스 엔지니어링
    파이썬 드라이버 (samsung_730b.py)
    C/libusb 드라이버 (samsung_730b.c)
    libfprint 드라이버 (samsung730b.c)

## 0. 전체 구조 요약

리눅스 표준 지문 스택(libfprint/fprintd/PAM) 계층 구조 :

```
[센서 하드웨어 (Samsung 730B)]
        ↑ USB (bulk + control, vendor-specific)
[로우레벨 드라이버 (파이썬 / libusb C)]
        ↑ 이미지/템플릿 API
[libfprint 드라이버 (samsung730b)]
        ↑ D-Bus
[fprintd]
        ↑ PAM / DE별 lock screen
[users]
```

## 1. 장치 / USB 기본 정보

- Vendor ID: 0x04E8 (Samsung)
- Product ID: 0x730B
- 인터페이스: Interface 0, Alternate Setting 0
    - Endpoints:
        - Bulk OUT: 0x01
        - Bulk IN : 0x82
    - Control transfer:
        - bmRequestType = 0x40 (Host → Device, Vendor, Device)
    - bRequest:
        - 0xC3: init
        - 0xCA: capture index / packet

참고: 이 값들은 파이썬 드라이버, C/libusb 드라이버, libfprint samsung730b 드라이버에서 모두 동일하게 사용중임

## 2. 초기 USB 시퀀스 (OS 레벨)

Windows(USBPcap 캡처)에서 vendor‑specific 프로토콜이 시작되기 전에, OS가 수행하는 일반적인 USB 열거 과정

```
GET_DESCRIPTOR (DEVICE, CONFIGURATION)
SET_CONFIGURATION
...
```
등 표준 USB 시퀀스가 있음.

libusb/pyusb에서 `set_configuration()` 호출 시 동일 역할 수행함

센서 프로토콜 관점에선 의미 없음

## 3. 센서 초기화

### 3.1 Control 초기화 (0xC3)

파이썬/ C 드라이버에서 공통으로 사용하는 초기 control 명령:

```py
ctrl_data = bytes([
	0x80, 0x84, 0x1e, 0x00,
	0x08, 0x00, 0x00, 0x01,
	0x01, 0x01, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00
])
self._ctrl_write(0xC3, 0x0000, 0x0000, ctrl_data)
```

libfprint 드라이버 대응 코드:

```c
guint8 c3_data[16] = {
  0x80, 0x84, 0x1e, 0x00, 0x08, 0x00, 0x00, 0x01,
  0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

usb_ctrl_out (usb_dev, 0xC3, 0x0000, 0x0000, c3_data, sizeof (c3_data),
              500, error);
```

- 레지스터 의미나 비트필드는 불명확하나, 동작상 "기본 모드/전원/클럭 설정" 정도로 추정함

### 3.2 0xA9 / 0xA8 레지스터 시퀀스 (bulk OUT)

Windows 730b.pcapng에서 공통적으로 등장하는 패턴:

- Bulk OUT length 31
- 마지막 4바이트가 항상 `0xA9` 또는 `0xA8`로 시작
- Wireshark에서 `Leftover Capture Data`로 표시되는 부분이 실제 센서 명령

패킷 예시:

```
URB_BULK out, length 31 ... Leftover Capture Data: a9 60 1b 00
URB_BULK out, length 31 ... Leftover Capture Data: a9 50 21 00
URB_BULK out, length 31 ... Leftover Capture Data: a9 61 00 00
```

파이썬/ C 드라이버에서는 이 4바이트만 떼서 그대로 Bulk OUT으로 전송함:

```python
init_cmds = [
    bytes([0x4f, 0x80]),
    bytes([0xa9, 0x4f, 0x80]),
    bytes([0xa9, 0xb9, 0x00]),
    bytes([0xa9, 0x60, 0x1b, 0x00]),
    bytes([0xa9, 0x50, 0x21, 0x00]),
    bytes([0xa9, 0x61, 0x00, 0x00]),
    ...
]

for cmd in init_cmds:
    ep_out.write(cmd)
```
- libfprint 드라이버의 init_cmds[]도 동일한 리스트를 사용
- 각 명령은 gain, timing, 노이즈 관련 레지스터 설정일듯?

## 4. 캡처 프로토콜

### 4.1 Windows 트레이스 기준 개요

USBPcap 캡처 기준 이미지 캡처 시퀀스 :

    Vendor control 요청 (0xCA, 가끔 0xCC)
    a8 06 00 00 (capture start) + zero padding이 포함된 큰 Bulk OUT
    그 뒤 반복되는 Bulk IN/OUT (이미지 데이터 블록으로 추정)

센서 입장에서 중요한 패턴:

    Index가 0x0100씩 증가하는 Control OUT
    캡처 시작 시점에 한 번 Bulk OUT a8 06 00 00 ...
    각 청크마다 Bulk IN 256 bytes (이미지 데이터)
    그 뒤 Bulk OUT 256 bytes of zeros (ACK)

## 4.2 파이썬 드라이버 캡처 [`samsung_730b.py`](../scripts/samsung_730b.py)

```python
CAPTURE_PACKET_SIZE = 256
CAPTURE_NUM_PACKETS = 85
CAPTURE_START_INDEX = 0x032A

def capture(self):
	...
	# ---------- 1) packet 0 : 상태 응답만 처리 ----------
	# wIndex = 0x032a (= CAPTURE_START_INDEX)
	wIndex0 = self.CAPTURE_START_INDEX
	self._ctrl_write(0xCA, 0x0003, wIndex0 & 0xFFFF, None)

	# 캡처 시작 명령 (a8 06 00 00 ...)
	capture_cmd = bytes([0xa8, 0x06, 0x00, 0x00]) + bytes(
		self.BULK_PACKET_SIZE - 4
	)
	self._bulk_out(capture_cmd)

	# 상태 응답(2byte)만 읽고 버림
	status = self._bulk_in(self.BULK_PACKET_SIZE, timeout=500)

	# ---------- 2) packet 1..N : 실제 이미지 데이터 ----------
	for i in range(1, self.CAPTURE_NUM_PACKETS):
		offset = i * self.CAPTURE_PACKET_SIZE
		wIndex = self.CAPTURE_START_INDEX + offset

		# CONTROL 0xCA: 프레임 패킷 설정
		self._ctrl_write(0xCA, 0x0003, wIndex & 0xFFFF, None)

		# 데이터 읽기 (256 bytes 기대)
		data = self._bulk_in(self.BULK_PACKET_SIZE, timeout=1000)
		if not data:
			break

		image_data.extend(data)
	...
```

## 4.3 캡처 버퍼 레이아웃과 유효 이미지 영역

여러 Offset 및 해상도 실험 결과 :

    전체 캡처 버퍼: 약 21,500 bytes
    유효 해상도: 112 x 96 (width x height), 8-bit grayscale
    Offset 최적값: 180 bytes (이 값을 써야 중앙 수직선이나 윗줄 노이즈같은게 없음)

정리:

    유효 지문 시작: buffer[180]
    크기: 112 * 96 = 10752 bytes

    libfprint 드라이버도 동일한 상수를 사용중

## 4.4 방향 (Orientation)

    - 센서 내부 좌표계의 "위쪽"이 이미지 버퍼상의 "오른쪽"에 매핑되어 있음
    - 사람 눈에 직관적으로 보이기 위해 파이썬 드라이버는 왼쪽으로 90도 회전(CCW)

    참고: 현재 libfprint 드라이버는 회전 없이 raw 레이아웃(112x96)을 그대로 FpImage로 넘기고 있음. 매칭 품질을 보며 회전 적용 여부를 검토해야 함

## 5. C/libusb 드라이버 [`samsung_730b.c`](../scripts/samsung_730b.c)

파이썬 드라이버를 libusb/C로 옮긴 드라이버

### 5.1 Chunk 0 동작 수정 (중요)

초기 C 버전은 모든 256바이트 Packet이 동일하다고 가정하여 Chunk 0에도 ACK를 보냈으나, 이는 타임아웃 오류(-7)를 유발했었고, 파이썬 로그와 usbmon 트레이스 비교 결과 정상 동작 찾아냄

- Packet 0 (Index 0x032A) :

        Control 0xCA, wIndex=0x032A, wValue=0x0003
        Bulk OUT 256 bytes (a8 06 00 00 + 0 padding)
        Bulk IN 256 요청 → 실제로는 0~2 bytes 정도만 들어옴 (상태 응답)
        중요: 이 응답은 버리고, ACK (Bulk OUT)를 보내지 않음.

- Packet 1..84 :

        Control 0xCA, wIndex = 0x032A + n * 0x0100
        Bulk IN 256 bytes (이미지 데이터)
        Bulk OUT 256 bytes of zeros (ACK)

이 패턴 수정 후 정상적인 지문 이미지를 획득함

### 5.2 Finger-detect

Finger detect를 위해 전체 85 청크 대신 DETECT_PACKETS (예: 6) 만큼만 캡처하는 경로를 따로 둠

    Packet 0 + 일부 Packet만 읽음
    데이터 버퍼의 0x00 / 0xFF 분포를 분석하는 Heuristic 사용

Heuristic 알고리즘 :

```py
# detect X: zeros 많고 ff비율 아주낮음
zero_ratio = zeros / total
ff_ratio = ff / total

# detect O: ff비율 80퍼 이상, zeros비율 1에 가깝지 않음
if ff_ratio > 0.3 and zeros < total * 0.95:
	return True
```

## 6. libfprint 드라이버 [`samsung730b.c`](https://gitlab.freedesktop.org/lignah/libfprint/-/blob/feature/samsung730b/libfprint/drivers/samsung730b.c?ref_type=heads)

### 6.1 libfprint (s730b_detect_finger)

Capture와 동일한 Control/Bulk 패턴을 사용하되, DETECT_PACKETS 만큼만 읽어서 Probe 버퍼로 사용했음

```c
static guint8
*s730b_detect_finger (GUsbDevice *usb_dev, gsize *out_len, GError **error)
{
  /* packet 0 처리 (생략) */
  ...
  /* packets 1..N: 데이터 수신 루프 */
  for (i = 1; i < DETECT_PACKETS; i++) {
      guint16 widx = CAPTURE_START_IDX + i * BULK_PACKET_SIZE;
      usb_ctrl_out (..., widx, ...);
      usb_bulk_in (...);
      usb_bulk_out (... zeros ACK ...);
  }
  ...
}
```
### 6.2 libfprint Heuristic (s730b_has_fingerprint)

```c
static gboolean
s730b_has_fingerprint (const guint8 *data, gsize len)
{
  if (!data || len < 512) return FALSE;

  /* 0x00과 0xFF의 개수 카운트 */
  // ...

  double ff_ratio = (double) ff / (double) total;

  /* 30% 이상이 0xFF이고, 전체가 0으로 가득 차 있지 않으면 지문감지 */
  if (ff_ratio > 0.30 && zeros < total * 0.95)
    return TRUE;

  return FALSE;
}
```

### 6.3 libfprint 대기 (s730b_wait_finger / s730b_wait_finger_lost)

    - s730b_wait_finger: "Init → Detect Finger → Heuristic" 루프를 반복 
    - 성공 시 로그를 출력하고 Full Capture 전에 센서를 다시 Init(리셋)
    - s730b_wait_finger_lost: Heuristic이 "손가락 없음"을 리포트할 때까지 대기 (Enroll/Verify 사이 처리용)

## 7. libfprint 드라이버의 Capture

C/libusb 드라이버의 로직 그대로 사용

    capture_indices[] : 파이썬/Windows 로그에서 관찰한 0x032a부터 시작하여 0x0100씩 증가하는 시퀀스를 그대로 사용

    전체 데이터 길이: 약 21.5 KB

## 8. libfprint로 이미지 넘기기

```c
static void
s730b_submit_image (FpImageDevice *dev, guint8 *raw, gsize raw_len)
{
  // ... (길이 검증) ...

  FpImage *img = fp_image_new (IMG_W, IMG_H);
  gsize img_size = 0;
  guchar *dst = (guchar *) fp_image_get_data (img, &img_size);

  memcpy (dst, raw + IMG_OFFSET, IMG_W * IMG_H);
  g_free (raw);

  fpi_image_device_report_finger_status (dev, TRUE);
  fpi_image_device_image_captured (dev, img);
}
```

현재 상태:

    Offset 180부터 112x96 바이트를 그대로 복사 (회전안함)

테스트 결과: 

    fprintd-enroll은 5/5 완료, fprintd-verify는 score 0/20

# 9. 요약 및 향후 계획

확정된 내용

    장치: Samsung 730B (VID 0x04E8, PID 0x730B)
    프로토콜: Control (0xC3/0xCA) 및 Bulk (0xA9/0xA8 Init, 데이터 청크 루프)
    캡처: Chunk 0 특수 처리 + 84 데이터 청크
    이미지: Offset 180, 112x96, 8bpp Grayscale (보기용 90도 회전 필요?)
    감지: 짧은 캡처(6 청크) + 0xFF/0x00 비율 Heuristic
    - libfprint 드라이버(samsung730b) 작성 완료
    - Enroll 동작 확인

다음으로 할 일 :

    [ ] fprintd-verify score 0/20 원인 분석/해결