updated 25.12.07

# samsung-730b-reverse

리눅스에서 Samsung 730B 지문인식을 위한 리버싱 노트, python/libusb 드라이버, libfprint 드라이버 작업용 저장소

- libfprint 브랜치(WIP 드라이버):  
  https://gitlab.freedesktop.org/lignah/libfprint/-/tree/feature/samsung730b?ref_type=heads

## 목적

- 삼성 노트북에서 리눅스 사용 시 지문인식이 안 됨
- Windows 드라이버 트래픽을 리버스해서
  - 파이썬/libusb 테스트 드라이버 작성
  - libfprint 공식 드라이버로 기여하는 것이 최종 목표

## 파이썬 드라이버 (v1.3 기준)

### 의존성

```bash
pip install pyusb pillow
```

### 사용법 (드라이버 v1.3)

```bash
sudo python scripts/samsung_730b_driver_v1_3.py --debug
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

## libfprint 드라이버 작업 상태 (요약)

별도 레포/브랜치에서 작업 중:

- libfprint 브랜치: `feature/samsung730b`
- MR: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/556

현재 구현:

- 새 드라이버 `samsung730b` 추가 (USB image device, press 타입)
- init 시퀀스:
  - control 0xC3 + Windows와 동일한 0xA9/0xA8 bulk OUT 시퀀스
- 이미지 캡처:
  - control 0xCA + 256B bulk IN/OUT, 85 청크
  - 파이썬 드라이버와 동일한 offset/해상도로 `FpImage` 구성
- finger detect:
  - 짧은 probe 캡처(6 청크) + ff 비율 기반 heuristic
  - 여러 round/재초기화 루프

상태:

- fprintd에서 장치가 `Samsung 730B (experimental)`로 잘 잡힘
- 손 올리면:
  - detect → capture → minutiae 추출까지 동작 확인됨
- 아직 fprintd enroll UX는 조금 더 튜닝 필요 (일부 프레임에서 minutiae 부족 경고)


## s730b_test.c (C/libusb 단독 테스트)

C/libusb로 구현한 [`s730b_v1_2.c`](scripts/s730b_v1_2.c)

빌드 예:

```bash
sudo pacman -S libusb
ls /usr/include/libusb-1.0/libusb.h

gcc -Wall -O2 s730b_v1_2.c -o s730b -lusb-1.0
sudo ./s730b_test
```

#### 잠시 학습시간
`-Wall` = 경고 많이 켜는 옵션 (버그잡기용)

`-O2` = 최적화 lv2. 빠르고 크기줄여 컴파일 (릴리즈용)

`gcc s730b_test.c -o s730b_test -lusb-1.0` 만 해도됨


## 참고

- 상세 프로토콜/드라이버 설계/pcap 분석/이미지 오프셋 찾은 과정 등:
  - [`protocol-samsung-730b.md`](./docs/protocol-samsung-730b.md) 참고하면 됨
- libfprint 드라이버는 freedesktop upstream 리뷰를 받으면서 계속 업데이트할 예정