updated 26.01.14

# samsung-730b-reverse

> **English documentation:** [`English-protocol-samsung-730b.md`](./docs/English-protocol-samsung-730b.md)

리눅스에서 Samsung 730B 지문인식을 위한 리버싱 노트, python/libusb 드라이버, libfprint 드라이버 작업용 저장소

*Reverse engineering notes, Python/libusb driver, and libfprint driver for Samsung 730B fingerprint sensor on Linux.*

- libfprint 브랜치: https://gitlab.freedesktop.org/lignah/libfprint/-/tree/feature/samsung730b
- **Merge Request:** https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/556

## 목적

- 삼성 노트북에서 리눅스 사용하는데 지문인식이 안 됨
- Windows 드라이버 트래픽 리버싱 후
  - 파이썬/libusb 드라이버 작성
  - [+] libfprint 공식 드라이버로 기여

## 파이썬 드라이버

Python/pyusb로 구현한 [`samsung_730b.py`](scripts/samsung_730b.py)

### 의존성

```bash
pip install pyusb pillow
```

### 사용법

```bash
sudo python scripts/samsung_730b.py --debug
```

기능:

- 센서 init (control 0xC3 + 0xA9/0xA8 시퀀스)
- 지문 이미지 캡처
- 이미지 기반 finger detect (간단한 heuristic)
- PNG로 저장

이미지 레이아웃 요약:

- 캡처 버퍼에서 유효 지문 시작 offset: **180 bytes** (구 버전 문서 182 → 180으로 확정)
- 해상도: **112 x 96**, 8bpp grayscale
- 보기 좋게 보려고 왼쪽으로 90도 회전함 (손톱이 위쪽으로 자라는 방향)

대략 코드:

```python
IMG_OFFSET = 180
IMG_W = 112
IMG_H = 96

img = Image.frombytes(
    "L",
    (IMG_W, IMG_H),
    data[IMG_OFFSET : IMG_OFFSET + IMG_W * IMG_H],
)
img = img.transpose(Image.ROTATE_90)
```

## C/libusb 드라이버

C/libusb로 구현한 [`samsung_730b.c`](scripts/samsung_730b.c)

빌드 예:

```bash
sudo pacman -S libusb
ls /usr/include/libusb-1.0/libusb.h

gcc -Wall -O2 samsung_730b.c -o samsung_730b -lusb-1.0
sudo ./samsung_730b
```

#### 잠시 학습시간

`-Wall` = 경고 많이 켜는 옵션 (버그잡기용)

`-O2` = 최적화 lv2. 빠르고 크기줄여 컴파일 (릴리즈용)

`gcc samsung_730b.c -o samsung_730b -lusb-1.0` 만 해도됨


## libfprint 드라이버 (완료)

MR 제출됨: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/556

### 센서 스펙

| 항목 | 값 |
|------|-----|
| VID/PID | 04e8:730b |
| 타입 | USB image device (press) |
| 이미지 크기 | 112×96 pixels |
| 해상도 | ~500 DPI (19.69 px/mm) |

### 구현 내용

- **Init 시퀀스:** control 0xC3 + 0xA9/0xA8 bulk OUT 시퀀스
- **이미지 캡처:** control 0xCA + 256B bulk IN/OUT, 85 패킷
- **Finger detect:** 짧은 캡처(6 패킷) + 0xFF 비율 기반 heuristic

### 이미지 전처리 파이프라인

1. **CLAHE** (Contrast Limited Adaptive Histogram Equalization) - clip_limit=3.0
2. **Contrast stretching** - 1st/99th percentile 기반
3. **Unsharp mask** - amount=2.5로 edge 강화
4. **2× upscaling** - NBIS minutiae 추출 정확도 향상 (224×192)

### 테스트 결과

| Metric | Result |
|--------|--------|
| True Accept Rate (TAR) | ~87% |
| False Accept Rate (FAR) | ~0-20% |
| bz3_threshold | 25 |


## 참고 문서

- 상세 프로토콜/드라이버 설계/pcap 분석/이미지 오프셋 찾은 과정:
  - 한국어: [`protocol-samsung-730b.md`](./docs/protocol-samsung-730b.md)
  - English: [`English-protocol-samsung-730b.md`](./docs/English-protocol-samsung-730b.md)