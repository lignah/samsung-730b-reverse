최종 업데이트 날짜: 25.12.07

# Samsung 730B 지문 센서 비공식 프로토콜 정리

> Samsung 730B (VID 0x04E8, PID 0x730B)에 대해 Windows 드라이버 트래픽과  
> 리눅스 리버스 엔지니어링 결과를 정리한 문서임  
> 파이썬 테스트 드라이버(v1.3)와 libfprint 드라이버 초안에서 검증한  
> 이미지 레이아웃/오프셋/손가락 감지 로직까지 반영했음  

---

## 0. 전체 구조 요약

리눅스 표준 지문 스택(libfprint/fprintd/PAM) 계층 구조 :

```text
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

이 문서는 제일 아래층인 “USB 프로토콜 + 이미지 레이아웃 + finger detect”를 정리한 문서임  

---

## 1. 장치/USB 기본 정보

- Vendor ID: `0x04E8` (Samsung)
- Product ID: `0x730B`
- 인터페이스:
  - Interface 0, Alternate Setting 0 사용
  - Endpoints:
    - Bulk OUT: `0x01`
    - Bulk IN : `0x82`
- Control transfer:
  - `bmRequestType = 0x40` (Host → Device, Vendor, Device)
  - 사용 bRequest:
    - `0xC3` : init 관련 설정
    - `0xCA` : 캡처 청크 인덱스 설정
    - 일부 실험 중인 다른 값들(`0xCC` 등)은 아직 확정 아님

---

## 2. 초기 USB 시퀀스 (OS 레벨)

Windows에서 730b.pcapng 기준, 장치 사용 직전에:

```text
GET DESCRIPTOR (DEVICE, CONFIGURATION)
SET CONFIGURATION
```

등 표준 USB 시퀀스가 있음.  
libusb/pyusb에서 `set_configuration()` 호출 시 동일 역할 수행함.  
센서 프로토콜 관점에선 의미 없음.

---

## 3. 센서 초기화 (Initialization)

### 3.1. Control 초기화 (0xC3)

파이썬/ C 드라이버에서 공통으로 사용하는 초기 control 명령:

```python
ctrl_data = bytes([
    0x80, 0x84, 0x1e, 0x00,
    0x08, 0x00, 0x00, 0x01,
    0x01, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00
])
dev.ctrl_transfer(0x40, 0xC3, 0x0000, 0x0000, ctrl_data)
```

특징:

- Windows pcap에서도 비슷한 control OUT이 보임
- 의미는 정확히 알 수 없지만 “센서 기본 모드/전원/클럭 설정” 정도로 추정함

### 3.2. Bulk OUT 기반 레지스터 시퀀스 (0xA9 / 0xA8)

Windows 730b.pcapng에서 공통적으로 등장하는 패턴:

- Bulk OUT length 31
- 마지막 4바이트가 항상 `0xA9` 또는 `0xA8`로 시작
- Wireshark에서 `Leftover Capture Data`로 표시되는 부분이 실제 센서 명령

예시:

```text
URB_BULK out, length 31
...
Leftover Capture Data: a9 60 1b 00

URB_BULK out, length 31
...
Leftover Capture Data: a9 50 21 00
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

libfprint 드라이버에서도 같은 시퀀스 사용 중임.

각 명령의 정확한 의미는 아직 미확정이고,  
주로 gain/노이즈/타이밍 관련 레지스터로 추정됨.

---

## 4. 이미지 캡처

### 4.1. 캡처 루프 개요

파이썬 드라이버 v1.3 기준:

- `CAPTURE_NUM_CHUNKS = 85`
- `BULK_PACKET_SIZE = 256`
- `CAPTURE_START_INDEX = 0x032A`

캡처 알고리즘(단순화):

```python
for i in range(CAPTURE_NUM_CHUNKS):
    offset = i * BULK_PACKET_SIZE
    wIndex = CAPTURE_START_INDEX + offset

    # 1) control 0xCA: 이번 청크의 인덱스 설정
    dev.ctrl_transfer(0x40, 0xCA, 0x0003, wIndex & 0xFFFF, None)

    # 2) 첫 청크일 때만 캡처 시작 커맨드 전송
    if i == 0:
        start_cmd = bytes([0xa8, 0x06, 0x00, 0x00]) + bytes(252)
        ep_out.write(start_cmd)

    # 3) 이미지 데이터 bulk IN
    data = ep_in.read(256, timeout=1000)
    image_data.extend(data)

    # 4) ACK (0으로 채운 256바이트)
    ep_out.write(bytes(256))
```

libfprint 드라이버 쪽 `s730b_capture_frame()`도 거의 동일 구조로 구현되어 있음.

### 4.2. 캡처 데이터 길이 / 레이아웃

실험 결과:

- 캡처 전체 버퍼 길이 ≈ 21kB (환경에 따라 약간 변동)
- 이 데이터를 1바이트 단위로 슬라이딩하면서 여러 해상도 테스트한 결과:

  - **112 x 96**에서만 지문처럼 자연스러운 무늬가 나왔음
  - 그 외 조합(56x96, 80x96 등)은 부자연스러움

- 특정 offset에서 세로줄/위쪽 노이즈가 사라지는 지점을 찾아서 최종 확정:

  - 유효 지문 시작 offset: **180 bytes**
  - 해상도: **112 x 96**

정리:

- raw 캡처 버퍼: `buf[0..N)`
- 유효 지문 영역: `buf[180 .. 180 + 112*96)`

파이썬 코드(v1.3):

```python
IMG_OFFSET = 180
IMG_W = 112
IMG_H = 96

img = Image.frombytes(
    "L",
    (IMG_W, IMG_H),
    buf[IMG_OFFSET : IMG_OFFSET + IMG_W * IMG_H],
)
```

libfprint 드라이버도:

```c
#define IMG_OFFSET 180
#define IMG_W      112
#define IMG_H      96

FpImage *img = fp_image_new(IMG_W, IMG_H);
gsize img_size = 0;
guchar *dst = (guchar *) fp_image_get_data (img, &img_size);

memcpy(dst, raw + IMG_OFFSET, IMG_W * IMG_H);
```

이렇게 동일하게 사용 중임.

### 4.3. 이미지 방향 (orientation)

센서 실제 방향 기준으로:

- 센서 상단에 “빈 공간”이 있고, 그쪽으로 손가락이 빠져나가는 구조임
- raw 이미지를 보면 이 빈 영역이 이미지 오른쪽에 나타남

그래서:

- 파이썬/GUI에서 보기 좋게 하려면 **왼쪽으로 90도 회전**하는 게 자연스러움

예:

```python
img = Image.frombytes("L", (IMG_W, IMG_H),
                      buf[IMG_OFFSET : IMG_OFFSET + IMG_W * IMG_H])
img = img.transpose(Image.ROTATE_90)
```

libfprint 쪽은 내부 매칭을 위해 굳이 회전 안 해도 되지만,  
향후 다른 드라이버들과 방향을 맞출지 검토 필요함.

---

## 5. finger detect 프로토콜 / heuristic

### 5.1. 프로브 캡처 (detect_probe)

finger detect 용으로 단축 캡처를 따로 둠:

- `DETECT_PROBE_CHUNKS = 6`
- 첫 청크는 캡처 시작 + 상태 읽기
- 나머지 5 청크에 대해 약간 줄인 IN/OUT 시퀀스를 사용

C 코드 개요(libfprint):

```c
guint8 *buf = g_malloc0(capacity);

usb_ctrl_out(..., CAPTURE_CTRL_REQ, CAPTURE_CTRL_VAL,
             CAPTURE_START_IDX, ...);

start_cmd[0] = 0xa8;
start_cmd[1] = 0x06;
usb_bulk_out(..., SAMSUNG730B_EP_OUT, start_cmd, sizeof(start_cmd), ...);

/* 첫 응답 읽기 */
usb_bulk_in(..., buf + total, BULK_PACKET_SIZE, DETECT_INIT_TIMEOUT_MS, &got);

/* 이후 1..DETECT_PROBE_CHUNKS-1 */
for (i = 1; i < DETECT_PROBE_CHUNKS; i++) {
    guint16 widx = CAPTURE_START_IDX + i * BULK_PACKET_SIZE;
    usb_ctrl_out(..., CAPTURE_CTRL_REQ, CAPTURE_CTRL_VAL, widx, ...);
    usb_bulk_in(...);       /* data 1개 */
    usb_bulk_out(...);      /* zero ACK */
}
```

파이썬에서도 비슷한 구조로 구현 가능함.

### 5.2. heuristic: 0xFF / 0x00 비율

단축 캡처 데이터를 바탕으로 간단한 finger detect heuristic을 사용함:

```c
static gboolean
s730b_has_finger (const guint8 *data, gsize len)
{
  if (!data || len < 512)
    return FALSE;

  gsize total = MIN (len, (gsize) 4096);
  gsize zeros = 0, ff = 0;

  for (gsize i = 0; i < total; i++)
    {
      guint8 v = data[i];
      if (v == 0x00)
        zeros++;
      else if (v == 0xFF)
        ff++;
    }

  double ff_ratio = (double) ff / (double) total;

  /* 경험적으로 맞춘 값:
   * - ff_ratio > 0.3
   * - zeros가 전체의 95% 미만
   */
  if (ff_ratio > 0.30 && zeros < total * 0.95)
    return TRUE;

  return FALSE;
}
```

프로브 한 번 결과로 finger present 여부를 판정함.

### 5.3. detect 루프 (wait_finger)

최종 detect 루프는 여러 라운드 + 여러 프로브로 구성됨:

```c
#define DETECT_MAX_ROUNDS        5
#define DETECT_PROBES_PER_ROUND  5
#define DETECT_INTERVAL_MS       400

static gboolean
s730b_wait_finger (GUsbDevice *usb_dev)
{
  for (guint r = 0; r < DETECT_MAX_ROUNDS; r++)
    {
      g_message ("s730b: detect round %u/%u", r + 1, DETECT_MAX_ROUNDS);

      for (guint i = 0; i < DETECT_PROBES_PER_ROUND; i++)
        {
          gsize probe_len = 0;
          GError *local_error = NULL;
          guint8 *probe = s730b_detect_probe (usb_dev, &probe_len, &local_error);

          if (!probe)
            {
              if (local_error)
                {
                  g_message ("s730b: detect probe error (round=%u, probe=%u): %s",
                             r, i, local_error->message);
                  g_clear_error (&local_error);
                }
              g_usleep ((gulong) DETECT_INTERVAL_MS * 1000);
              continue;
            }

          gboolean finger = s730b_has_finger (probe, probe_len);
          g_free (probe);

          g_message ("s730b: probe %u: finger=%d", i, finger);
          if (finger)
            return TRUE;

          g_usleep ((gulong) DETECT_INTERVAL_MS * 1000);
        }

      g_message ("s730b: re-init after failed detect round");
      /* init 재실행 (에러는 soft-fail로 취급) */
      ...
    }

  g_message ("s730b: finger detect timeout (no finger)");
  return FALSE;
}
```

특징:

- USB 에러/타임아웃은 상위로 에러를 올리지 않고 log만 남김
- detect 실패 시 `FALSE`만 반환해서 libfprint/fprintd 쪽에서  
  “이번 activate에는 손가락이 없었다” 정도로 처리하게 함

---

## 6. libfprint C 드라이버 설계/구현 상태

### 6.1. ID 테이블

```c
#define SAMSUNG730B_VID 0x04e8
#define SAMSUNG730B_PID 0x730b

static const FpIdEntry samsung730b_ids[] = {
  { .vid = SAMSUNG730B_VID, .pid = SAMSUNG730B_PID, .driver_data = 0 },
  { .vid = 0,               .pid = 0,               .driver_data = 0 },
};
```

### 6.2. 디바이스 클래스 설정

```c
dev_class->id = "samsung730b";
dev_class->full_name = "Samsung 730B (experimental)";
dev_class->id_table = samsung730b_ids;
dev_class->type = FP_DEVICE_TYPE_USB;
dev_class->scan_type = FP_SCAN_TYPE_PRESS;

img_class->img_open = samsung730b_dev_init;
img_class->img_close = samsung730b_dev_deinit;
img_class->activate = samsung730b_dev_activate;
img_class->deactivate = samsung730b_dev_deactivate;

img_class->img_width = IMG_W;
img_class->img_height = IMG_H;
img_class->bz3_threshold = 20;
```

### 6.3. activate() 흐름

```c
static void
samsung730b_dev_activate (FpImageDevice *dev)
{
  GUsbDevice *usb = fpi_device_get_usb_device (FP_DEVICE (dev));
  GError *error = NULL;
  guint8 *raw = NULL;
  gsize raw_len = 0;

  /* 1) finger detect */
  if (!s730b_wait_finger (usb))
    {
      fpi_image_device_report_finger_status (dev, FALSE);
      fpi_image_device_activate_complete (dev, NULL);
      return;
    }

  /* 2) detect 후 init 재실행 */
  if (!s730b_run_init (usb, &error))
    {
      fpi_image_device_activate_complete (dev, error);
      return;
    }

  /* 3) full frame capture */
  if (!s730b_capture_frame (usb, &raw, &raw_len, &error))
    {
      fpi_image_device_activate_complete (dev, error);
      return;
    }

  fpi_image_device_activate_complete (dev, NULL);
  s730b_emit_image (dev, raw, raw_len);
}
```

` s730b_emit_image()`에서:

- `raw_len` 길이 검사
- `IMG_OFFSET` 이후 `IMG_W * IMG_H` 만큼 잘라서 `FpImage`에 복사
- `fpi_image_device_report_finger_status(dev, TRUE);`
- `fpi_image_device_image_captured(dev, img);`

까지 호출함.

---

## 7. 에러 / 타임아웃 / 향후 과제

### 7.1. 현재까지 확인한 이슈

- 일부 캡처 프레임에서 libfprint가  
  `Failed to detect minutiae: No minutiae found` 경고를 남김
- fprintd enroll UX가 아직 완전히 매끄럽지 않음:
  - 처음 몇 프레임에서 minutiae 부족 → enroll 단계가 안 진행되거나
  - fprintd 쪽 세그폴트(로컬 빌드/환경 이슈일 가능성)도 간헐적으로 확인

### 7.2. 향후 작업 아이디어

- 이미지 품질 개선:
  - init 시퀀스 0xA9/0xA8 값 약간씩 바꿔가며 SNR/대비 비교
  - 오프셋/윈도우 약간 조정해서 잡음 줄이기
- libfprint 통합 튜닝:
  - 다른 이미지 드라이버들이 어떻게 activate/deactivate/finger-off를 다루는지 비교
  - 삼성 730B 드라이버도 동일 패턴으로 맞추기
- Windows pcap 비교:
  - 손가락 없음/실패 시나리오 pcap 추가로 캡처
  - Windows 드라이버가 타임아웃/에러 후 어떤 리셋 시퀀스를 쓰는지 확인

---

## 8. 요약 (현재까지 확정된 포인트)

1. **장치 정보**
   - VID/PID: 0x04E8 / 0x730B
   - Bulk OUT/IN: 0x01 / 0x82
   - Control OUT: 0x40, bRequest=0xC3/0xCA 등

2. **초기화**
   - Control 0xC3 + 0xA9/0xA8 bulk OUT 시퀀스
   - Windows pcap의 `Leftover Capture Data`와 파이썬/libfprint 코드 1:1 매칭

3. **이미지 캡처**
   - 256B x 85 청크, control 0xCA + bulk IN/OUT
   - 유효 지문 영역:
     - offset: **180 bytes**
     - size: **112 x 96**, 8bpp
   - 필요 시 보기용으로 90도 회전

4. **finger detect**
   - 단축 캡처(6 청크) + ff/zero 비율 heuristic
   - 여러 프로브/라운드 + 재init 루프

5. **libfprint 드라이버**
   - `Samsung 730B (experimental)` 이미지 디바이스로 동작
   - detect/capture/minutiae까지 통과 확인
   - fprintd enroll UX/이미지 품질 관련 튜닝은 앞으로 더 해야 함

이 문서는 파이썬/ C 테스트 코드와 libfprint 드라이버 작업을 위한  
레퍼런스로 계속 업데이트할 예정임