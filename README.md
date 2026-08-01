updated 26.08.01

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


## libfprint 드라이버

MR: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/556  
브랜치: https://gitlab.freedesktop.org/lignah/libfprint/-/tree/feature/samsung730b

### 진행 상태 (2026-08-01)

| 항목 | 상태 |
|------|------|
| 드라이버 초안 | 완료, MR 제출 |
| 최신 `master` 리베이스 | 완료 |
| umockdev capture 테스트 | 완료 · MR 반영 (`tests/samsung730b/`) |
| 매칭률 튜닝 | 로컬 완료 · **libfprint 브랜치 푸시 전** (아래 파라미터) |

리뷰 요청 (Marco Trevisan): umockdev 테스트 추가 → 반영함.  
`meson test samsung730b` 로컬 통과 (~9s).

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
- **멀티프레임:** 후보 9장 중 ROI score 최고 선택 후 제출

### 이미지 전처리 파이프라인

1. **CLAHE** (Contrast Limited Adaptive Histogram Equalization) — `clip_limit=3.0`
2. **Contrast stretching** — 1st/99th percentile 기반
3. **Unsharp mask** — `amount=1.5` (구버전 2.5 → 완화, FAR 감소)
4. **2× upscaling** — NBIS minutiae용 224×192, `ppmm` 동반 조정

### 매칭 파라미터 (튜닝 후)

| 파라미터 | 구버전 (MR 초안) | **현재 (로컬)** | 비고 |
|----------|------------------|-----------------|------|
| `bz3_threshold` | 25 | **32** | thr 낮으면 타인 오인↑ |
| unsharp amount | 2.5 | **1.5** | 과도한 샤픈 → 가짜 minutiae |
| CLAHE clip | 3.0 | 3.0 | 2.0으로 내리면 TAR 붕괴 |
| enroll sat255 한도 | 42% | **52%** | 등록 무한 retry 완화 |
| enroll min grad | 20 | **18** | 동일 |
| finger-off wait (verify 후) | 1200 ms | **2800 ms** | “손 떼라” 에러 감소 |
| finger-already-on wait | 2000 ms | **3500 ms** | 동일 |

변경 파일 (libfprint): `libfprint/drivers/samsung730b.c`  
→ GitLab `feature/samsung730b` 에 커밋·푸시는 별도 진행.

### 매칭 측정 결과 (실기기, 2026-08-01)

평가: enroll 5 stages → genuine 10회 + impostor 5회  
도구: [`scripts/match_eval.py`](scripts/match_eval.py)

| 설정 | TAR (본인) | FAR (타인) | 본인 점수 | 타인 점수 |
|------|------------|------------|-----------|-----------|
| thr=25 (구, 재측정) | ~9/10 (90%) | **~2/5 (40%)** | — | — |
| thr=32 + unsharp 1.5 | **10/10 (100%)** | **0/5 (0%)** | 39–62 | 18–29 |
| thr=41 | 7/10 (70%) | 0/5 (0%) | thr 과다로 본인 탈락 | 낮음 |
| CLAHE 2.0 + unsharp 1.0 | 4/10 (40%) | 0/5 | 점수 붕괴 | 낮음 |

**채택:** thr=32, unsharp=1.5, CLAHE=3.0 + enroll/손뗌 타이밍 조정.  
본인 최저 ~39 / 타인 최고 ~29 / thr 32 → 마진 확보.

구문서 수치 (소표본): TAR ~87% (7/8), FAR 0/5, thr=25 — 참고용. 상세는 [`docs/protocol-samsung-730b.md`](./docs/protocol-samsung-730b.md) §10.

### umockdev 테스트

- 경로 (libfprint): `tests/samsung730b/{device,capture.pcapng,capture.png}`
- 드라이버 시퀀스/이미지가 바뀌면 재캡처 필요할 수 있음:
  ```bash
  # libfprint build dir
  sudo ./tests/create-driver-test.py samsung730b
  meson test -C build samsung730b
  ```

## 매칭 평가 스크립트

[`scripts/match_eval.py`](scripts/match_eval.py) — 로컬 빌드 libfprint로 enroll / 본인 / 타인 측정.

```bash
# 예: 빌드된 libfprint 경로
export LIBDIR=/path/to/libfprint/build/libfprint
sudo -E env LD_LIBRARY_PATH="$LIBDIR" GI_TYPELIB_PATH="$LIBDIR" G_MESSAGES_DEBUG=all \
  python3 scripts/match_eval.py \
    --libdir "$LIBDIR" \
    --genuine 10 --impostor 5 \
    --outdir results/run-$(date +%Y%m%d-%H%M%S) \
    --config "my-config-label"
```

- `outdir/STATUS` — 현재 할 일 한 줄 (손가락 타이밍)
- `outdir/progress.log` — 사람용 진행 로그
- `outdir/baseline.json` / `baseline.md` — TAR/FAR·점수 요약

측정 로그 예시(로컬): `results/` (git에 안 넣을 수 있음).

## 참고 문서

- 상세 프로토콜/드라이버 설계/pcap 분석/이미지 오프셋 찾은 과정:
  - 한국어: [`protocol-samsung-730b.md`](./docs/protocol-samsung-730b.md)
  - English: [`English-protocol-samsung-730b.md`](./docs/English-protocol-samsung-730b.md)
