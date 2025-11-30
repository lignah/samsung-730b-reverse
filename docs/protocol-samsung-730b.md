최종 업데이트 날짜: 25.11.30

# Samsung 730B 지문 센서 비공식 프로토콜 정리

> Samsung 730B (VID 0x04E8, PID 0x730B)에 대해 Windows 드라이버 트래픽과  
> 리눅스 리버스 엔지니어링 결과를 정리한 문서임  
> 파이썬 테스트 드라이버(v1.1)에서 확인된 이미지 레이아웃/오프셋/방향 정보까지 반영했음  

---

## 0. 전체 구조 요약

리눅스 표준 지문 스택(libfprint/fprintd/PAM) 계층 구조 :

```text
[센서 하드웨어 (Samsung 730B)]
        ↑ USB (bulk + control, vendor-specific)
[로우레벨 드라이버 (파이썬 → libusb C)]
        ↑ 이미지/템플릿 API
[libfprint 드라이버]
        ↑ D-Bus
[fprintd]
        ↑ PAM / DE별 lock screen
[users]
```

이 문서는 제일 아래층인 “USB 프로토콜 + 이미지 레이아웃”을 정리했음  

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
  - `bRequest = 0xC3`, `0xCA`, `0xCC` 등 사용 (vendor-specific)

리눅스 파이썬 드라이버에서는 위 엔드포인트/요청 값 기준으로 동작함

---

## 2. 초기 USB 시퀀스 (OS 레벨)

Windows에서 730b.pcapng기준, 장치 사용 직전에 다음과 같은 표준 USB 시퀀스가 있음:

```text
GET DESCRIPTOR Request/Response (DEVICE, CONFIGURATION)
SET CONFIGURATION Request/Response
```

표준 USB 동작 -> driver protocol의 관점에선 특별한 의미가 없었음  
리눅스에서 `libusb` / `pyusb`의 `set_configuration()`이 동일 역할 수행

---

## 3. 센서 초기화 (Initialization)

### 3.1. Control 초기화 (0xC3)

리눅스 파이썬 드라이버에서 사용하는 초기 control 명령:

```python
ctrl_data = bytes([
    0x80, 0x84, 0x1e, 0x00,
    0x08, 0x00, 0x00, 0x01,
    0x01, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00
])
dev.ctrl_transfer(0x40, 0xC3, 0x0000, 0x0000, ctrl_data)
```

- Windows pcap에서도 대응되는 control OUT/IN 패킷이 존재
- 정확한 의미(레지스터 맵, 비트 필드)는 아직 미확정
  - “센서 기본 설정 / 모드 전환” 정도로 추정됨

---

## 4. 패턴1: 0xA9 레지스터 쓰기 (Bulk OUT length 31)

### 4.1. Windows 쪽 패킷 구조

730b.pcapng에서 관찰된 “패턴1” 구조:

- Bulk OUT, length = 31 bytes
- 예시 (일부):

```text
URB_BULK out, length 31
0000   1b 00 20 7a da c2 88 e7 ff ff 00 00 00 00 09 00
0010   00 02 00 03 00 01 03 04 00 00 00 a9 60 1b 00
                                          ^^^^^^^^^^
                                          Leftover Capture Data: a9 60 1b 00

URB_BULK out, length 31
0000   1b 00 20 fa 26 c7 88 e7 ff ff 00 00 00 00 09 00
0010   00 02 00 03 00 01 03 04 00 00 00 a9 50 21 00
                                          ^^^^^^^^^^
                                          Leftover Capture Data: a9 50 21 00

URB_BULK out, length 31
0000   1b 00 20 fa 26 c7 88 e7 ff ff 00 00 00 00 09 00
0010   00 02 00 03 00 01 03 04 00 00 00 a9 61 00 00
                                          ^^^^^^^^^^
                                          Leftover Capture Data: a9 61 00 00
```

공통점:

- 앞쪽 27바이트 (`1b 00 20 ... 00 00 00`)는  
  Windows 드라이버/USB 스택이 쓰는 래핑/메타데이터로 보임
- 마지막 4바이트가 항상 `0xA9`로 시작하는 “센서 명령” 부분  
  → Wireshark에서 `Leftover Capture Data`로 따로 표시됨

### 4.2. 리눅스 드라이버에서의 사용 방식

파이썬 드라이버 `_init_sensor()`에서는 이 마지막 4바이트만 떼서 센서에 직접 전송함:

```python
init_cmds = [
    bytes([0x4f, 0x80]),
    bytes([0xa9, 0x4f, 0x80]),
    bytes([0xa8, 0xb9, 0x00]),
    bytes([0xa9, 0x60, 0x1b, 0x00]),
    bytes([0xa9, 0x50, 0x21, 0x00]),
    bytes([0xa9, 0x61, 0x00, 0x00]),
    ...
]
for cmd in init_cmds:
    ep_out.write(cmd)  # Windows leftover data 4바이트만 전송하는 형태
```

실제로 이렇게 보내도 센서가 정상 동작하는 걸로 보아,  
센서 내부에서는 **0xA9 명령 3~4바이트만 의미 있는 페이로드**라고 결론낼 수 있음  

### 4.3. 0xA9 명령 의미 추정 테이블 (초기화 시퀀스 기준)

| 순서 | 명령 바이트             | 길이 | 추정 역할(가설)              | 비고              |
|------|-------------------------|------|-------------------------------|-------------------|
| 0    | `4f 80`                 | 2    | soft reset or mode set?      | bulk len=2        |
| 1    | `a9 4f 80`              | 3    | config register write?       |                   |
| 2    | `a8 b9 00`              | 3    | 또 다른 config or mode       |                   |
| 3    | `a9 60 1b 00`           | 4    | gain or timing 관련?         | leftover 예시     |
| 4    | `a9 50 21 00`           | 4    | ???                           | leftover 예시     |
| 5    | `a9 61 00 00`           | 4    | ???                           | leftover 예시     |
| ...  | ...                     | ...  | ...                           |                   |

TODO:

- [ ] 각 0xA9 명령에 대해 값을 약간씩 바꿔보고 이미지 품질/밝기/노이즈 변화를 관찰해서 역할 추정
---

## 5. 이미지 캡처 (패턴3 + control 0xCA)

### 5.1. Windows 측 패턴 개요

730b.pcapng에서 지문 이미지 캡처 구간으로 보이는 부분 :

- 처음으로 큰 Bulk OUT (length = 286)
- 이어지는 Bulk IN/OUT (length = 283/286) 반복 (패턴3)
- 중간에 Control OUT (`0xCA`, `0xCC` 추정)과 조합

대략 구조:

```text
[control 0xCA, 0xCC 등 설정]
[bulk out: leftover a8 06 00 00 + padding 0x00]   ← 캡처 시작
[bulk in/out 27 bytes + 283 bytes 반복]            ← 이미지 프레임 데이터 스트림
...
```

패턴3 요약:

- 27바이트 BULK in/out + 283바이트 BULK in/out을 반복하면서  
  센서로부터 이미지 데이터 블록을 받아오는 흐름으로 보임  

### 5.2. 리눅스 파이썬 드라이버의 캡처 구현

파이썬 드라이버 `capture()`는 다음과 같이 동작함 :

```python
CAPTURE_CHUNK_SIZE = 256
CAPTURE_NUM_CHUNKS = 85
CAPTURE_START_INDEX = 0x032A

image_data = bytearray()

for i in range(CAPTURE_NUM_CHUNKS):
    offset = i * CAPTURE_CHUNK_SIZE
    wIndex = CAPTURE_START_INDEX + offset

    # 1) CONTROL 0xCA: 프레임 청크 설정
    dev.ctrl_transfer(0x40, 0xCA, 0x0003, wIndex & 0xFFFF, None)

    # 2) 첫 청크에서만 캡처 시작 명령 (0xA8 06 00 00 + padding)
    if i == 0:
        capture_cmd = bytes([0xa8, 0x06, 0x00, 0x00]) + bytes(252)
        ep_out.write(capture_cmd)

    # 3) 데이터 bulk IN (256 bytes)
    data = ep_in.read(256, timeout=500)
    image_data.extend(data)

    # 4) ACK (전부 0으로 채워진 256 bytes)
    ep_out.write(bytes(256))
```

특징:

- 현재 캡처 데이터 길이(실측): **대략 21506 bytes**
  - 이 값은 환경/타이밍에 따라 약간 변동 가능성 있음
- 이 전체 데이터에서, 실제 유효 지문 영역은 아래와 같이 추출함 (v1.1 기준):

  - **유효 지문 시작 오프셋(`BEST_OFFSET`) = 182 bytes**
  - **이미지 크기 = 112 x 96 = 10752 bytes**
  - 즉 실제 지문 프레임은 `data[182 : 182 + 112*96]`

### 5.3. 이미지 레이아웃/오프셋 (실험 결과)

여러 해상도/오프셋 실험 결과:

- 해상도 후보 중 `112 x 96`에서만 지문 무늬가 자연스럽게 보였음
- `56x96`, `80x96`, `96x112` 등은 지문처럼 보이지 않았음
- `112x96`을 `data[:112*96]`으로 그렸을 때:
  - 가운데 세로 줄(경계) + 맨 윗줄 가로 노이즈 존재
- offset을 바꿔가며 (특히 1바이트 단위로 64~72, 이후 112 단위 등) 스캔한 결과:
  - **offset 70** 부근에서 세로줄이 거의 사라지는 프레임 발견 (아직 맨 위의 가로줄 존재)
  - 추가로 한 줄(112 bytes) 더 밀어 **offset 182**로 그렸을 때:
    - 세로줄 없음
    - 맨 위 가로 노이즈도 사라진 깔끔한 지문 프레임 확인됨

정리:

- **센서 캡처 한 번에 들어오는 row x col: 112 x 96 (그레이스케일, 8bpp)**  
- **유효 지문 데이터의 시작 오프셋 = 182 bytes**  
- 그 이전/이후의 영역은:
  - 내부 헤더거나 패딩 혹은 두 번째 프레임 일부일 가능성 있음 (아직 분류 안 됨)

파이썬 드라이버에서 최종 이미지 생성 로직(v1.1):

```python
BEST_OFFSET = 182
IMAGE_WIDTH = 112
IMAGE_HEIGHT = 96

img = Image.frombytes(
    "L",
    (IMAGE_WIDTH, IMAGE_HEIGHT),
    data[BEST_OFFSET : BEST_OFFSET + IMAGE_WIDTH * IMAGE_HEIGHT],
)
```

### 5.4. 이미지 방향(orientation)

센서 물리 구조 :

- 센서 윗쪽을 비워 두고 손가락을 올리면,
  - 실제 센서 좌표계 기준으로는 “윗쪽 영역이 검게(비어있게)” 나와야 함
- 하지만 offset/해상도를 맞춘 상태에서 출력된 이미지를 보면:
  - 빈 영역이 **이미지 오른쪽**에 위치함
  - 즉, 현재 메모리 레이아웃 기준으로는 센서의 “위쪽”이 이미지 상 “오른쪽 끝”에 해당

이걸 사람의 기준으로 :

- 이미지를 **왼쪽으로 90도 회전**하면,
  - 센서 위쪽 빈 영역이 이미지 맨 위로 이동함
  - 손가락 방향/센서 방향이 직관적으로 맞게 정렬됨

파이썬 드라이버 v1.1에서 사용하는 회전 코드:

```python
# offset + 112x96 적용 후
img = Image.frombytes('L', (112, 96), data[182 : 182 + 112*96])

# 센서 실제 방향에 맞게 왼쪽으로 90도 회전
img = img.transpose(Image.ROTATE_90)
```

정리:

- **raw 데이터 기준 정규 좌표계**: 112(가로) x 96(세로), offset 182  
- **사람/GUI용 보기 좋은 방향**: 위 코드를 통해 90도 왼쪽으로(CCW) 회전한 결과  

---

## 6. 파이썬 테스트 드라이버(v1.1) 레이아웃 요약

파이썬 드라이버 v1.1에서 사용하는 핵심 상수/로직을 표로 정리함:

| 항목                    | 값/설명                                               |
|-------------------------|-------------------------------------------------------|
| VID / PID              | 0x04E8 / 0x730B                                       |
| Bulk OUT / IN EP       | 0x01 / 0x82                                           |
| BULK_PACKET_SIZE       | 256 bytes                                             |
| CHUNK_SIZE / NUM       | 256 bytes x 85                                        |
| CAPTURE_START_INDEX    | 0x032A (control 0xCA의 wIndex 시작값)                |
| 캡처 데이터 총 길이    | ≈ 21506 bytes (환경에 따라 약간 변동)                |
| 유효 지문 offset       | **182 bytes**                                         |
| 유효 지문 크기         | **112 x 96 = 10752 bytes**                            |
| 이미지 포맷            | Grayscale 8bpp (`'L'`), `Image.frombytes('L', ...)`   |
| 보기용 방향            | 왼쪽으로 90도 회전 (`transpose(Image.ROTATE_90)`)     |
| 손가락 감지            | 아직 미해결, 현재는 사용자 입력 기반 수동 모드만 구현 |

이 레이아웃은:

- 나중에 libusb C 테스트 바이너리에서 동일하게 쓸 수 있고
- libfprint 드라이버에서 `struct fp_img` 채울 때도 그대로 참고하면 됨  

---

## 7. libfprint C 드라이버 설계 초안

아직 구현 전이지만, 파이썬 드라이버를 바탕으로  
libfprint용 C 드라이버를 어떻게 구성하면 좋을지 적어둔 뼈대

### 7.1. 장치 매칭

```c
static const struct usb_id samsung730b_ids[] = {
    { .vendor = 0x04e8, .product = 0x730b },
    { 0, 0, 0, 0 },
};
```

### 7.2. 드라이버 구조체 스켈레톤

(libfprint 버전에 따라 이름/필드는 다를 수 있고 이건 대략적인 그림임)

```c
struct fp_img_driver samsung730b_driver = {
    .driver = {
        .id = FP_DRIVER_ID_SAMSUNG730B,
        .name = "samsung-730b",
        .full_name = "Samsung 730B fingerprint reader",
        .usb_id = samsung730b_ids,
        .type = DRIVER_IMAGING,
    },
    .open = samsung730b_open,
    .close = samsung730b_close,
    .capture = samsung730b_capture,
    .enroll = samsung730b_enroll,
    .verify = samsung730b_verify,
    // ...
};
```

### 7.3. open() 내부에서 할 일

- libusb로 장치 찾기/열기
- interface claim
- bulk in/out endpoint 찾기
- `_init_sensor()`에 해당하는 초기화 시퀀스 실행 :
  - control 0xC3
  - 0xA9/0xA8 시퀀스

파이썬 `_init_sensor()`를 그대로 libusb 호출로 옮기는 느낌으로

### 7.4. capture() 내부에서 할 일

- `samsung_730b_driver_v1_1.py`의 `capture()`와 동일 로직:

  - for i in 0..84:
    - control 0xCA (wValue=0x0003, wIndex=0x032A + 256*i)
    - 첫 chunk면 bulk out `a8 06 00 00 + padding`
    - bulk in 256 bytes → 버퍼에 append
    - bulk out 256 bytes zero (ACK)

- 캡처 버퍼가 준비되면:

  - `offset = 182`
  - `width = 112`, `height = 96`
  - `data = buffer + offset`

- libfprint의 이미지 객체로 변환:

```c
struct fp_img *img = fp_img_new(width, height);
memcpy(img->data, buffer + 182, width * height);
// 이미지 회전 처리가 필요할지도?
return img;
```

- 이미지 회전 처리 :
  - 파이썬에서는 사람이 보기 좋게 90도 회전했지만 libfprint에선 어떨지 아직모름
  - 이 부분은 libfprint내의 다른 드라이버 구현들을 참고해야겠음

---

## 8. 손가락 감지(Finger Detection) 상태

### 8.1. 현재 상태

- `0xA8 0x20 0x00 0x00` 명령은 finger detect가 아닌 것으로 판단됨:
  - 손가락 유무에 따라 응답이 달라지지 않음 (non_zero=0 고정)
- 730b.pcapng에서도 손가락을 올리기 직전/직후에  
  확실히 구분되는 명령/응답 패턴은 아직 못 찾음

### 8.2. 임시 전략 (파이썬/향후 C)

- 파이썬 드라이버:
  - `wait_finger_manual()`로 사용자 입력 기반 수동 대기만 제공
- libfprint 드라이버 초안:
  - 초기 버전에서는 finger detect없이 바로 capture시도하는 방식으로 구현해볼거임

향후 할 일:

- [ ] “손가락 안 올리고 시간 초과된 세션” .pcap 하나 더 만들기
- [ ] 현재 730b.pcapng(손가락 올려서 성공)과 비교해서  
      손가락 올리기 전/후 트래픽 차이 좁히기
- [ ] 특정 control/bulk 응답에서 finger present flag가 있는지 찾고  
      이를 기반으로 `wait_finger()` 구현하기

---

## 9. 에러 / 타임아웃 / 리셋 관련

### 9.1. 현재 동작

- 파이썬 드라이버에서:
  - bulk IN 타임아웃 발생 시 캡처 루프 중단 → 실패 처리
- 별도의 soft reset / 재초기화 루틴은 아직 없음

### 9.2. 향후 설계 아이디어

- 윈도우 pcap에서 에러/재시도 시퀀스를 찾아보고,  
  아래 같은 루틴을 설계할 수 있음:

  - `samsung730b_reset_soft()`:
    - 일부 0xA9/0xA8 시퀀스를 다시 보내서 센서를 초기 상태로 되돌림
  - capture 실패가 연속 N번 발생하면:
    - soft reset 호출
    - 그래도 안 되면 libfprint에 에러 반환  

TODO:

- [ ] 타임아웃 재현 후, 동일 구간 윈도우 pcap과 비교
- [ ] Windows 드라이버가 비슷한 상황에서 어떤 control/bulk 시퀀스를 보내는지 확인  

---

## 10. 요약 (현재까지 확정된 포인트)

1. **장치 정보**
   - VID: `0x04E8`
   - PID: `0x730B`
   - Bulk OUT: `0x01`, Bulk IN: `0x82`
   - Control OUT: `bmRequestType=0x40`, `bRequest=0xC3 / 0xCA` 등 사용

2. **초기화**
   - Control 0xC3 + 0xA9/0xA8 기반 레지스터 설정 시퀀스  
   - Windows 730b.pcapng의 Leftover Capture Data와 파이썬 코드가 1:1 매칭됨

3. **이미지 캡처**
   - 캡처 루프: 256B x 85 청크 (실제 유효 데이터 길이는 환경에 따라 ≈21506 bytes 확인)
   - 각 청크마다 Control 0xCA + Bulk IN/OUT (ACK 포함) 조합
   - **유효 지문 영역**:
     - 시작 오프셋: `182` bytes
     - 해상도: `112 x 96` (grayscale, 8bpp)
     - 방향: 출력이미지 기준으로는 센서 위쪽이 이미지 오른쪽에 위치  
       → 보기용 이미지는 왼쪽으로 90도 회전해서 사용하는 것이 자연스러움

4. **손가락 감지**
   - 아직 USB 레벨에서 확실히 파악되지 않음
   - 현재 파이썬 드라이버는 수동입력 기반의 wait_finger만 있음 (TODO)
   - libfprint 드라이버 초기 버전은 “finger detect 없이 캡처만”으로 시작

5. **향후 작업 우선순위**
   1. libusb C 테스트 프로그램에서 파이썬과 동일한 캡처/이미지 레이아웃 재현  
   2. libfprint 드라이버 스켈레톤 구현 (open/close/capture)  
   3. Windows vs Linux pcap 비교를 통한 finger detect 프로토콜 추가 분석  
   4. fprintd/PAM/Hyprlock 통합 및 다른 사용자 배포(AUR 패키지 등)