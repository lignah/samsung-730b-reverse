/*
 * s730b_test.c
 *
 * - Samsung 730B를 libusb로 잡아서 캡처 시도하는 실험용 코드
 * - 현재(25.11.30) 기준:
 *   - 센서 초기화(init_sensor)는 python driver v1.1과 동일하게 동작하는 것 같음
 *   - 캡처(capture_frame)는 chunk=0에서 bulk ACK 타임아웃 나면서
 *     전체 길이가 2~3 bytes 수준으로만 들어오는 WIP 상태임
 *
 *  [*] 캡처 시작...
 *  [-] bulk ACK 실패 chunk=0, err=-7
 *  [+] 캡처 완료: 2 bytes, non-zero=0 bytes
 *  [+] RAW 저장됨: capture.raw
 *  [+] s730b_test 종료
 * 
 * - 원인?:
 *   - Windows/pylibusb 시퀀스와 libusb C 시퀀스의 미세한 차이(패킷크기나 타이밍)가
 *     남아있어서 센서가 캡처 모드로 안 들어간 듯
 * - TODO:
 *   - [완료] python driver캡처 시 usbmon으로 패킷 덤프 
 *   - [완료] 동일 조건에서 s730b_test실행 후 usbmon 덤프
 *   - [완료] 두 pcap 비교해서 control 0xCA / bulk OUT(a8 06..) / bulk IN/ACK 패턴을
 *           1:1로 맞춘 뒤 다시 테스트 해봐야할듯
 * 
 * ------------ 25.12.02 -------------------
 * 
 * 문제 찾음
 *     요약 : 파이썬 코드는 걍 운이 좋았음..
 *           chunk[0] == 상태응답, chunk[1] ~ chunk[84] == 데이터
 *           파이썬에서는 이 구분을 제대로 하지 않아도 알아서 처리해준것같은 느낌
 *
 * - 파이썬 코드는 "모든 청크가 동일한 256바이트 데이터"라고 가정했음
 *   (for 루프에서 매 chunk마다 ctrl 0xCA → bulk IN 256 → bulk OUT 256(ACK) 하는 식)
 *
 * - usbmon과 파이썬코드의 _ctrl_write()에 wIndex를 찍어본 결과
 *   실제 프로토콜은 다음과 같이 동작함:
 *
 *   1) chunk 0 (wIndex=0x032a)
 *      - control 0xCA, wValue=0x0003, wIndex=0x032a
 *      - bulk OUT 256바이트: a8 06 00 00 ... (캡처 시작 명령)
 *      - bulk IN 256 요청을 보내도 실제로는 0~2바이트(0x0000 정도)만 들어옴
 *      - "캡처모드 진입 ack"
 *
 *   2) chunk 1..84 (wIndex=0x042a..0x572a)
 *      - control 0xCA (wIndex를 0x032a + n*0x0100, n=1..84로 올림)
 *      - bulk IN 256바이트: 실제 지문 이미지/메타데이터 청크
 *      - bulk OUT 256바이트 0x00: ACK
 *      - 이 패턴이 84번 반복되면서 약 21504바이트가 쌓임
 *
 * - 처음 C코드는 이 구조를 제대로 몰랐었고,
 *   chunk[0]의 2바이트를 일반 데이터로 취급 -> 바로 256바이트 ACK를 날렸음
 *   
 *   → 센서 : "뭐임 데이터 아직 보내지도 않았는데 ACK가 오네?"
 *     이후 bulk IN이 2바이트에서 끊겨 청크 전송모드로 진입하지 못함
 *     내 ack도 timeout (err=-7) 나면서 캡처가 중단됨
 *
 * - 그래서 :
 *   - chunk 0은 따로 분리해서
 *       * control 0xCA + a8 06 00 00 ...
 *       * bulk IN으로 0~2바이트 상태응답만 읽고 버림
 *       * ACK를 보내지 않음
 *   - chunk 1..84에서만
 *       * control 0xCA (python에서 찍은 wIndex시퀀스 그대로 사용)
 *       * bulk IN 256바이트를 이미지 데이터로 누적
 *       * bulk OUT 256바이트 0x00을 ACK로 전송
 *   이런 식으로 python driver와 최대한 동일한 패턴으로 맞췄음
 *
 * - 이 변경 이후에는
 *   - 총 캡처 길이가 약 21504 bytes로 나오고
 *     python(raw_to_png.py)에서 png로 만들어 보면
 *     정상적인 지문 이미지가 보이는 것을 확인했음
 *
 */

/*
 * 프로토콜 요약 (25.12.02):
 *
 * - 장치 정보
 *   - VID/PID: 0x04e8 / 0x730b
 *   - BULK OUT EP: 0x01
 *   - BULK IN  EP: 0x82
 *
 * - 캡처 시퀀스 개요
 *   1) 센서 초기화
 *      - control 0xC3 한 번 + 0xA9/0xA8 init 시퀀스 여러 개
 *
 *   2) 캡처 시작 (chunk 0: 상태 응답)
 *      - control 0xCA, wValue=0x0003, wIndex=0x032a
 *      - bulk OUT 256 bytes: 첫 4바이트가 a8 06 00 00, 나머지는 0 채움
 *      - bulk IN 256 bytes 요청하면 실제로는 0~2 bytes (0x0000) 정도만 들어옴 → "초기 상태 응답"으로 간주하고 버림
 *      - 이 chunk 0에서는 이미지 데이터 누적도 안 하고, ACK(256 zeros)도 보내지 않음
 *
 *   3) 실제 이미지 데이터 (chunk 1..84)
 *      - control 0xCA를 wIndex=0x032a + n*0x0100 (n=1..84)로 반복 호출
 *        -> python driver에서 찍은 wIndex시퀀스를 그대로 하드코딩한 테이블(capture_indices[]) 사용
 *      - 각 chunk마다:
 *          * bulk IN 256 bytes (지문 이미지 청크)
 *          * 읽은 256 bytes를 버퍼에 붙임
 *          * bulk OUT 256 bytes of zeros (ACK) 전송
 *      - 이렇게 총 84개의 256B 청크를 받아서 약 21504 bytes정도 누적
 *
 * - 이미지 레이아웃 (python driver v1.2)
 *   - 전체 캡처 데이터 길이: 대략 21504 bytes
 *   - 유효 지문 영역:
 *       * offset 180부터 112 x 96 (가로 x 세로, 8bit grayscale)
 *       * 센서의 실제방향 기준으론 왼쪽으로 90도 회전해야 사람이 보기편한 방향이 됨
 *   - python에서는 offset=180, width=112, height=96, rotate=90deg로 png를 만들고 있음
 *   - capture.raw는 순수한 112 x 96 지문이미지만 있는 파일이 아니라 앞뒤에 이것저것 섞여있는 바이트스트림
 *     그중에 가장 선명한 지문이미지는 180 offset (2바이트 해결전에는 182 offset이었음)
 *
 * 이 파일은 libfprint에 드라이버를 추가할 때,
 * - "센서 init 시퀀스"
 * - "capture 시퀀스 (0xCA/0xA8/0x82/0x01 패턴)"
 * - "이미지 오프셋/해상도 정보"
 * 를 C쪽에서 어떻게 구현했는지 레퍼런스로 쓰기 위한 테스트 코드임
 */

/*
 * s730b_test.c
 *
 * - Samsung 730B를 libusb로 잡아서 캡처/손가락 감지 테스트하는 코드임
 * - python driver v1.3 + libfprint 드라이버에서 쓴 프로토콜을 C로 재현함
 *
 * 기능:
 *   - 센서 초기화 (control 0xC3 + 0xA9/0xA8 시퀀스)
 *   - finger detect용 짧은 캡처 (heuristic)
 *   - full frame 캡처 후 RAW/PGM 저장
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#define SAMSUNG730B_VID 0x04e8
#define SAMSUNG730B_PID 0x730b

#define BULK_EP_OUT 0x01
#define BULK_EP_IN  0x82
#define BULK_PACKET_SIZE 256

#define IMG_OFFSET 180
#define IMG_WIDTH  112
#define IMG_HEIGHT 96

static int
save_pgm_from_raw (const unsigned char *raw,
                   int                  raw_len,
                   const char          *fname,
                   int                  rotate_90)
{
    int needed = IMG_OFFSET + IMG_WIDTH * IMG_HEIGHT;
    if (raw_len < needed) {
        fprintf (stderr, "[-] RAW 길이가 너무 짧음 (len=%d, 필요=%d)\n", raw_len, needed);
        return -1;
    }

    const unsigned char *src = raw + IMG_OFFSET;

    int w = IMG_WIDTH;
    int h = IMG_HEIGHT;

    const unsigned char *img_data = NULL;
    unsigned char *rotated = NULL;

    if (!rotate_90) {
        img_data = src;
    } else {
        rotated = malloc (w * h);
        if (!rotated) {
            fprintf (stderr, "[-] rotate용 메모리 malloc 실패함\n");
            return -1;
        }

        /* 왼쪽으로 90도 회전 */
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int src_idx = y * w + x;
                int x2 = y;
                int y2 = (w - 1 - x);
                int dst_idx = y2 * h + x2;
                rotated[dst_idx] = src[src_idx];
            }
        }

        int tmp = w;
        w = h;
        h = tmp;
        img_data = rotated;
    }

    FILE *f = fopen (fname, "wb");
    if (!f) {
        fprintf (stderr, "[-] %s 열기 실패\n", fname);
        free (rotated);
        return -1;
    }

    fprintf (f, "P5\n%d %d\n255\n", w, h);

    size_t img_size = (size_t) (w * h);
    if (fwrite (img_data, 1, img_size, f) != img_size) {
        fprintf (stderr, "[-] PGM 데이터 쓰기 실패\n");
        fclose (f);
        free (rotated);
        return -1;
    }

    fclose (f);
    free (rotated);

    printf ("[+] PGM 저장됨: %s (width=%d, height=%d, rotate_90=%d)\n",
            fname, w, h, rotate_90);
    return 0;
}

/* 파이썬에서 찍은 0xCA wIndex 시퀀스 (n = 0..84) */
static const uint16_t capture_indices[] = {
    0x032a, 0x042a, 0x052a, 0x062a,
    0x072a, 0x082a, 0x092a, 0x0a2a,
    0x0b2a, 0x0c2a, 0x0d2a, 0x0e2a,
    0x0f2a, 0x102a, 0x112a, 0x122a,
    0x132a, 0x142a, 0x152a, 0x162a,
    0x172a, 0x182a, 0x192a, 0x1a2a,
    0x1b2a, 0x1c2a, 0x1d2a, 0x1e2a,
    0x1f2a, 0x202a, 0x212a, 0x222a,
    0x232a, 0x242a, 0x252a, 0x262a,
    0x272a, 0x282a, 0x292a, 0x2a2a,
    0x2b2a, 0x2c2a, 0x2d2a, 0x2e2a,
    0x2f2a, 0x302a, 0x312a, 0x322a,
    0x332a, 0x342a, 0x352a, 0x362a,
    0x372a, 0x382a, 0x392a, 0x3a2a,
    0x3b2a, 0x3c2a, 0x3d2a, 0x3e2a,
    0x3f2a, 0x402a, 0x412a, 0x422a,
    0x432a, 0x442a, 0x452a, 0x462a,
    0x472a, 0x482a, 0x492a, 0x4a2a,
    0x4b2a, 0x4c2a, 0x4d2a, 0x4e2a,
    0x4f2a, 0x502a, 0x512a, 0x522a,
    0x532a, 0x542a, 0x552a, 0x562a,
    0x572a,
};
static const size_t CAPTURE_NUM_CHUNKS =
    sizeof (capture_indices) / sizeof (capture_indices[0]);

static void
die (const char *msg, int err)
{
    if (err < 0)
        fprintf (stderr, "[-] %s (err=%d)\n", msg, err);
    else
        fprintf (stderr, "[-] %s\n", msg);
    exit (1);
}

/* init용 명령 정의 */
struct cmd_def {
    const unsigned char *data;
    size_t               len;
};

/* init 시퀀스 (Windows leftover data / python v1.3 기반) */
static const unsigned char cmd0[]  = {0x4f, 0x80};
static const unsigned char cmd1[]  = {0xa9, 0x4f, 0x80};
static const unsigned char cmd2[]  = {0xa8, 0xb9, 0x00};
static const unsigned char cmd3[]  = {0xa9, 0x60, 0x1b, 0x00};
static const unsigned char cmd4[]  = {0xa9, 0x50, 0x21, 0x00};
static const unsigned char cmd5[]  = {0xa9, 0x61, 0x00, 0x00};
static const unsigned char cmd6[]  = {0xa9, 0x62, 0x00, 0x1a};
static const unsigned char cmd7[]  = {0xa9, 0x63, 0x00, 0x1a};
static const unsigned char cmd8[]  = {0xa9, 0x64, 0x04, 0x0a};
static const unsigned char cmd9[]  = {0xa9, 0x66, 0x0f, 0x80};
static const unsigned char cmd10[] = {0xa9, 0x67, 0x1b, 0x00};
static const unsigned char cmd11[] = {0xa9, 0x68, 0x00, 0x0f};
static const unsigned char cmd12[] = {0xa9, 0x69, 0x00, 0x14};
static const unsigned char cmd13[] = {0xa9, 0x6a, 0x00, 0x19};
static const unsigned char cmd14[] = {0xa9, 0x6c, 0x00, 0x19};
static const unsigned char cmd15[] = {0xa9, 0x40, 0x43, 0x00};
static const unsigned char cmd16[] = {0xa9, 0x41, 0x6f, 0x00};
static const unsigned char cmd17[] = {0xa9, 0x55, 0x20, 0x00};
static const unsigned char cmd18[] = {0xa9, 0x5f, 0x00, 0x00};
static const unsigned char cmd19[] = {0xa9, 0x52, 0x27, 0x00};
static const unsigned char cmd20[] = {0xa9, 0x09, 0x00, 0x00};
static const unsigned char cmd21[] = {0xa9, 0x5d, 0x4d, 0x00};
static const unsigned char cmd22[] = {0xa9, 0x51, 0xa8, 0x25};
static const unsigned char cmd23[] = {0xa9, 0x03, 0x00};
static const unsigned char cmd24[] = {0xa9, 0x38, 0x01, 0x00};
static const unsigned char cmd25[] = {0xa9, 0x3d, 0xff, 0x0f};
static const unsigned char cmd26[] = {0xa9, 0x10, 0x60, 0x00};
static const unsigned char cmd27[] = {0xa9, 0x3b, 0x14, 0x00};
static const unsigned char cmd28[] = {0xa9, 0x2f, 0xf6, 0xff};
static const unsigned char cmd29[] = {0xa9, 0x09, 0x00, 0x00};
static const unsigned char cmd30[] = {0xa9, 0x0c, 0x00};
static const unsigned char cmd31[] = {0xa8, 0x20, 0x00, 0x00};
static const unsigned char cmd32[] = {0xa9, 0x04, 0x00};
static const unsigned char cmd33[] = {0xa8, 0x08, 0x00};
static const unsigned char cmd34[] = {0xa9, 0x09, 0x00, 0x00};
static const unsigned char cmd35[] = {0xa8, 0x3e, 0x00, 0x00};
static const unsigned char cmd36[] = {0xa9, 0x03, 0x00, 0x00};
static const unsigned char cmd37[] = {0xa8, 0x20, 0x00, 0x00};
static const unsigned char cmd38[] = {0xa9, 0x10, 0x00, 0x01};
static const unsigned char cmd39[] = {0xa9, 0x2f, 0xef, 0x00};
static const unsigned char cmd40[] = {0xa9, 0x09, 0x00, 0x00};
static const unsigned char cmd41[] = {0xa9, 0x5d, 0x4d, 0x00};
static const unsigned char cmd42[] = {0xa9, 0x51, 0x3a, 0x25};
static const unsigned char cmd43[] = {0xa9, 0x0c, 0x00};
static const unsigned char cmd44[] = {0xa8, 0x20, 0x00, 0x00};
static const unsigned char cmd45[] = {0xa9, 0x04, 0x00, 0x00};
static const unsigned char cmd46[] = {0xa9, 0x09, 0x00, 0x00};

static const struct cmd_def init_cmds[] = {
    { cmd0,  sizeof cmd0  },
    { cmd1,  sizeof cmd1  },
    { cmd2,  sizeof cmd2  },
    { cmd3,  sizeof cmd3  },
    { cmd4,  sizeof cmd4  },
    { cmd5,  sizeof cmd5  },
    { cmd6,  sizeof cmd6  },
    { cmd7,  sizeof cmd7  },
    { cmd8,  sizeof cmd8  },
    { cmd9,  sizeof cmd9  },
    { cmd10, sizeof cmd10 },
    { cmd11, sizeof cmd11 },
    { cmd12, sizeof cmd12 },
    { cmd13, sizeof cmd13 },
    { cmd14, sizeof cmd14 },
    { cmd15, sizeof cmd15 },
    { cmd16, sizeof cmd16 },
    { cmd17, sizeof cmd17 },
    { cmd18, sizeof cmd18 },
    { cmd19, sizeof cmd19 },
    { cmd20, sizeof cmd20 },
    { cmd21, sizeof cmd21 },
    { cmd22, sizeof cmd22 },
    { cmd23, sizeof cmd23 },
    { cmd24, sizeof cmd24 },
    { cmd25, sizeof cmd25 },
    { cmd26, sizeof cmd26 },
    { cmd27, sizeof cmd27 },
    { cmd28, sizeof cmd28 },
    { cmd29, sizeof cmd29 },
    { cmd30, sizeof cmd30 },
    { cmd31, sizeof cmd31 },
    { cmd32, sizeof cmd32 },
    { cmd33, sizeof cmd33 },
    { cmd34, sizeof cmd34 },
    { cmd35, sizeof cmd35 },
    { cmd36, sizeof cmd36 },
    { cmd37, sizeof cmd37 },
    { cmd38, sizeof cmd38 },
    { cmd39, sizeof cmd39 },
    { cmd40, sizeof cmd40 },
    { cmd41, sizeof cmd41 },
    { cmd42, sizeof cmd42 },
    { cmd43, sizeof cmd43 },
    { cmd44, sizeof cmd44 },
    { cmd45, sizeof cmd45 },
    { cmd46, sizeof cmd46 },
};
static const size_t init_cmds_len =
    sizeof (init_cmds) / sizeof (init_cmds[0]);

static void
init_sensor (libusb_device_handle *dev)
{
    int r;

    /* 1) control 0xC3 */
    unsigned char c3_data[16] = {
        0x80, 0x84, 0x1e, 0x00,
        0x08, 0x00, 0x00, 0x01,
        0x01, 0x01, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00
    };

    r = libusb_control_transfer (dev,
                                 0x40,
                                 0xC3,
                                 0x0000,
                                 0x0000,
                                 c3_data,
                                 sizeof c3_data,
                                 500);
    if (r < 0)
        die ("control 0xC3 전송 실패", r);

    /* 2) 0xA9/0xA8 init 시퀀스 */
    for (size_t i = 0; i < init_cmds_len; i++) {
        const unsigned char *cmd = init_cmds[i].data;
        size_t len = init_cmds[i].len;

        int transferred = 0;
        r = libusb_bulk_transfer (dev,
                                  BULK_EP_OUT,
                                  (unsigned char *) cmd,
                                  (int) len,
                                  &transferred,
                                  500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] init bulk 전송 실패 idx=%zu, err=%d\n", i, r);
            die ("init 실패", r);
        }
    }
}

/* full frame 캡처 */
static int
capture_frame (libusb_device_handle *dev,
               unsigned char       **out_buf,
               int                  *out_len)
{
    int r;
    int transferred;
    int total_len = 0;
    int capacity = (int) CAPTURE_NUM_CHUNKS * BULK_PACKET_SIZE + 1024;
    unsigned char *buf = malloc (capacity);

    if (!buf)
        die ("malloc 실패", -1);

    /* chunk 0: 상태 응답만 */
    {
        uint16_t wIndex0 = capture_indices[0];

        r = libusb_control_transfer (dev,
                                     0x40,
                                     0xCA,
                                     0x0003,
                                     wIndex0,
                                     NULL,
                                     0,
                                     500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] control 0xCA 실패 chunk=0, err=%d\n", r);
            goto out_fail;
        }

        unsigned char start_cmd[256];
        memset (start_cmd, 0, sizeof start_cmd);
        start_cmd[0] = 0xa8;
        start_cmd[1] = 0x06;
        start_cmd[2] = 0x00;
        start_cmd[3] = 0x00;

        r = libusb_bulk_transfer (dev,
                                  BULK_EP_OUT,
                                  start_cmd,
                                  sizeof start_cmd,
                                  &transferred,
                                  500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] 캡처 시작 bulk 전송 실패 (chunk=0, err=%d)\n", r);
            goto out_fail;
        }

        unsigned char tmp0[256];
        r = libusb_bulk_transfer (dev,
                                  BULK_EP_IN,
                                  tmp0,
                                  sizeof tmp0,
                                  &transferred,
                                  500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] 초기 상태 bulk IN 실패 chunk=0, err=%d\n", r);
            goto out_fail;
        }
        printf ("[*] 초기 상태 응답: %d bytes\n", transferred);
    }

    /* chunk 1..N: 실제 데이터 + ACK */
    for (size_t i = 1; i < CAPTURE_NUM_CHUNKS; i++) {
        uint16_t wIndex = capture_indices[i];

        r = libusb_control_transfer (dev,
                                     0x40,
                                     0xCA,
                                     0x0003,
                                     wIndex,
                                     NULL,
                                     0,
                                     500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] control 0xCA 실패 chunk=%zu, err=%d\n", i, r);
            break;
        }

        unsigned char tmp[256];
        r = libusb_bulk_transfer (dev,
                                  BULK_EP_IN,
                                  tmp,
                                  sizeof tmp,
                                  &transferred,
                                  1000);
        if (r < 0) {
            fprintf (stderr,
                     "[-] bulk IN 실패 chunk=%zu, err=%d\n", i, r);
            break;
        }
        if (transferred == 0) {
            fprintf (stderr,
                     "[*] chunk=%zu 에서 0 bytes, 종료함\n", i);
            break;
        }

        if (total_len + transferred > capacity) {
            capacity *= 2;
            buf = realloc (buf, capacity);
            if (!buf)
                die ("realloc 실패임", -1);
        }
        memcpy (buf + total_len, tmp, transferred);
        total_len += transferred;

        unsigned char ack[256] = {0};
        r = libusb_bulk_transfer (dev,
                                  BULK_EP_OUT,
                                  ack,
                                  sizeof ack,
                                  &transferred,
                                  500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] bulk ACK 실패 chunk=%zu, err=%d\n", i, r);
            break;
        }
    }

    *out_buf = buf;
    *out_len = total_len;
    return 0;

out_fail:
    free (buf);
    *out_buf = NULL;
    *out_len = 0;
    return -1;
}

/* finger detect용 짧은 캡처 (python finger_detect랑 비슷하게) */
static int
detect_probe (libusb_device_handle *dev,
              unsigned char       **out_buf,
              int                  *out_len,
              int                   max_chunks)
{
    int r;
    int transferred;
    int total_len = 0;
    int capacity = max_chunks * BULK_PACKET_SIZE + 256;
    unsigned char *buf = malloc (capacity);

    if (!buf)
        die ("detect malloc 실패", -1);

    /* chunk 0: 상태 응답 */
    {
        uint16_t wIndex0 = capture_indices[0];

        r = libusb_control_transfer (dev,
                                     0x40,
                                     0xCA,
                                     0x0003,
                                     wIndex0,
                                     NULL,
                                     0,
                                     500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] detect: control 0xCA 실패 chunk=0, err=%d\n", r);
            goto out_fail;
        }

        unsigned char start_cmd[256];
        memset (start_cmd, 0, sizeof start_cmd);
        start_cmd[0] = 0xa8;
        start_cmd[1] = 0x06;
        start_cmd[2] = 0x00;
        start_cmd[3] = 0x00;

        r = libusb_bulk_transfer (dev,
                                  BULK_EP_OUT,
                                  start_cmd,
                                  sizeof start_cmd,
                                  &transferred,
                                  500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] detect: 캡처 시작 bulk 전송 실패 chunk=0, err=%d\n", r);
            goto out_fail;
        }

        unsigned char tmp0[256];
        r = libusb_bulk_transfer (dev,
                                  BULK_EP_IN,
                                  tmp0,
                                  sizeof tmp0,
                                  &transferred,
                                  500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] detect: 초기 상태 bulk IN 실패 chunk=0, err=%d\n", r);
            goto out_fail;
        }
        /* 상태응답은 일단 버퍼에 그냥 누적 */
        if (total_len + transferred > capacity) {
            capacity *= 2;
            buf = realloc (buf, capacity);
            if (!buf)
                die ("detect realloc 실패", -1);
        }
        memcpy (buf + total_len, tmp0, transferred);
        total_len += transferred;
    }

    /* chunk 1..max_chunks-1 */
    for (int i = 1; i < max_chunks; i++) {
        uint16_t wIndex = capture_indices[i];

        r = libusb_control_transfer (dev,
                                     0x40,
                                     0xCA,
                                     0x0003,
                                     wIndex,
                                     NULL,
                                     0,
                                     500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] detect: control 0xCA 실패 chunk=%d, err=%d\n",
                     i, r);
            break;
        }

        unsigned char tmp[256];
        r = libusb_bulk_transfer (dev,
                                  BULK_EP_IN,
                                  tmp,
                                  sizeof tmp,
                                  &transferred,
                                  700);
        if (r < 0) {
            fprintf (stderr,
                     "[-] detect: bulk IN 실패 chunk=%d, err=%d\n", i, r);
            break;
        }
        if (transferred == 0) {
            fprintf (stderr,
                     "[*] detect: chunk=%d 에서 0 bytes, 종료\n", i);
            break;
        }

        if (total_len + transferred > capacity) {
            capacity *= 2;
            buf = realloc (buf, capacity);
            if (!buf)
                die ("detect realloc 실패", -1);
        }
        memcpy (buf + total_len, tmp, transferred);
        total_len += transferred;

        /* ACK */
        unsigned char ack[256] = {0};
        r = libusb_bulk_transfer (dev,
                                  BULK_EP_OUT,
                                  ack,
                                  sizeof ack,
                                  &transferred,
                                  500);
        if (r < 0) {
            fprintf (stderr,
                     "[-] detect: bulk ACK 실패 chunk=%d, err=%d\n", i, r);
            break;
        }
    }

    *out_buf = buf;
    *out_len = total_len;
    return 0;

out_fail:
    free (buf);
    *out_buf = NULL;
    *out_len = 0;
    return -1;
}

/* python _has_finger_in_detect 와 같은 heuristic */
static int
has_finger_in_detect (const unsigned char *data, int len)
{
    if (!data || len < 512) {
        fprintf (stderr,
                 "[detect] data too short for finger detect (len=%d)\n", len);
        return 0;
    }

    int total = len < 4096 ? len : 4096;
    int zeros = 0;
    int ff = 0;

    for (int i = 0; i < total; i++) {
        unsigned char v = data[i];
        if (v == 0x00)
            zeros++;
        else if (v == 0xFF)
            ff++;
    }

    double zero_ratio = (double) zeros / (double) total;
    double ff_ratio = (double) ff / (double) total;

    fprintf (stderr,
             "[detect] stats: total=%d, zeros=%d (%.2f), ff=%d (%.2f)\n",
             total, zeros, zero_ratio, ff, ff_ratio);

    if (ff_ratio > 0.30 && zeros < total * 0.95)
        return 1;

    return 0;
}

int
main (int argc, char **argv)
{
    int r;
    libusb_device_handle *dev = NULL;

    printf ("========================================\n");
    printf ("       samsung 730b libusb test\n");
    printf ("========================================\n\n");

    r = libusb_init (NULL);
    if (r < 0)
        die ("libusb_init 실패", r);

    dev = libusb_open_device_with_vid_pid (NULL,
                                           SAMSUNG730B_VID,
                                           SAMSUNG730B_PID);
    if (!dev)
        die ("장치를 찾을 수 없음 (VID/PID or sudo..?)", -1);

    if (libusb_kernel_driver_active (dev, 0) == 1) {
        r = libusb_detach_kernel_driver (dev, 0);
        if (r < 0)
            die ("커널 드라이버 분리 실패", r);
    }

    r = libusb_set_configuration (dev, 1);
    if (r < 0)
        die ("set_configuration 실패", r);

    r = libusb_claim_interface (dev, 0);
    if (r < 0)
        die ("claim_interface 실패", r);

    printf ("[*] 센서 초기화 중...\n");
    init_sensor (dev);
    printf ("[+] 센서 초기화 완료\n\n");

    /* finger detect 루프 */
    const int max_rounds = 5;
    const int probes_per_round = 5;
    int finger_ok = 0;

    for (int round = 0; round < max_rounds && !finger_ok; round++) {
        printf ("[*] finger detect round %d/%d...\n",
                round + 1, max_rounds);

        for (int i = 0; i < probes_per_round; i++) {
            unsigned char *detect_buf = NULL;
            int detect_len = 0;

            r = detect_probe (dev, &detect_buf, &detect_len, 6);
            if (r == 0 && detect_buf) {
                int finger = has_finger_in_detect (detect_buf, detect_len);
                free (detect_buf);

                if (finger) {
                    printf ("[+] finger detected by heuristic "
                            "(round=%d, probe=%d)\n", round + 1, i + 1);
                    finger_ok = 1;
                    break;
                }
            } else {
                printf ("[-] detect_probe 실패 (round=%d, probe=%d)\n",
                        round + 1, i + 1);
            }

            /* 짧게 쉬고 다시 detect */
            struct timespec ts = {0};
            ts.tv_sec = 0;
            ts.tv_nsec = 400 * 1000 * 1000; /* 400ms */
            nanosleep (&ts, NULL);
        }

        if (!finger_ok) {
            printf ("[*] 이 라운드에서 finger 없음, 센서 재초기화 시도\n");
            /* 간단히 다시 init만 호출 (close/open까지는 안 함) */
            init_sensor (dev);
        }
    }

    if (!finger_ok) {
        printf ("[-] finger detect timeout, 종료함\n");
        libusb_release_interface (dev, 0);
        libusb_close (dev);
        libusb_exit (NULL);
        return 1;
    }

    /* 여기까지 오면 손가락 있다고 보고 바로 full frame 캡처 */
    printf ("[*] full frame 캡처 시작...\n");
    unsigned char *buf = NULL;
    int len = 0;
    r = capture_frame (dev, &buf, &len);
    if (r < 0 || !buf)
        die ("캡처 실패", r);

    int non_zero = 0;
    for (int i = 0; i < len; i++)
        if (buf[i] != 0) non_zero++;

    printf ("[+] 캡처 완료: %d bytes, non-zero=%d bytes\n", len, non_zero);

    const char *fname = "capture.raw";
    FILE *f = fopen (fname, "wb");
    if (!f)
        die ("capture.raw 열기 실패", -1);
    fwrite (buf, 1, len, f);
    fclose (f);
    printf ("[+] RAW 저장됨: %s\n", fname);

    save_pgm_from_raw (buf, len, "capture.pgm", 1);

    free (buf);

    libusb_release_interface (dev, 0);
    libusb_close (dev);
    libusb_exit (NULL);

    printf ("[+] s730b_test 종료됨\n");
    return 0;
}