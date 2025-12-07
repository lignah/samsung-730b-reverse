#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libusb-1.0/libusb.h>

#define SAMSUNG730B_VID 0x04e8
#define SAMSUNG730B_PID 0x730b

#define BULK_EP_OUT 0x01
#define BULK_EP_IN  0x82
#define BULK_PACKET_SIZE 256

#define IMG_OFFSET 180
#define IMG_WIDTH  112
#define IMG_HEIGHT 96

static int save_pgm_from_raw(const unsigned char *raw, int raw_len, const char *fname, int rotate_90) {
    int needed = IMG_OFFSET + IMG_WIDTH * IMG_HEIGHT;
    if (raw_len < needed) {
        fprintf(stderr, "[-] RAW 길이가 너무 짧음 (len=%d, 필요=%d)\n", raw_len, needed);
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
        rotated = malloc(w * h);
        if (!rotated) {
            fprintf(stderr, "[-] rotate용 메모리 malloc 실패함\n");
            return -1;
        }

        // 왼쪽으로 90도 회전
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

    FILE *f = fopen(fname, "wb");
    if (!f) {
        fprintf(stderr, "[-] %s 열기 실패\n", fname);
        free(rotated);
        return -1;
    }

    fprintf(f, "P5\n%d %d\n255\n", w, h);

    size_t img_size = (size_t)(w * h);
    if (fwrite(img_data, 1, img_size, f) != img_size) {
        fprintf(stderr, "[-] PGM 데이터 쓰기 실패\n");
        fclose(f);
        free(rotated);
        return -1;
    }

    fclose(f);
    free(rotated);

    printf("[+] PGM 저장됨: %s (width=%d, height=%d, rotate_90=%d)\n",
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
static const size_t CAPTURE_NUM_CHUNKS = sizeof(capture_indices) / sizeof(capture_indices[0]);

static void die(const char *msg, int err) {
    if (err < 0)
        fprintf(stderr, "[-] %s (err=%d)\n", msg, err);
    else
        fprintf(stderr, "[-] %s\n", msg);
    exit(1);
}

/* init용 명령 정의 */
struct cmd_def {
    const unsigned char *data;
    size_t len;
};

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
    { cmd0,  sizeof(cmd0)  },
    { cmd1,  sizeof(cmd1)  },
    { cmd2,  sizeof(cmd2)  },
    { cmd3,  sizeof(cmd3)  },
    { cmd4,  sizeof(cmd4)  },
    { cmd5,  sizeof(cmd5)  },
    { cmd6,  sizeof(cmd6)  },
    { cmd7,  sizeof(cmd7)  },
    { cmd8,  sizeof(cmd8)  },
    { cmd9,  sizeof(cmd9)  },
    { cmd10, sizeof(cmd10) },
    { cmd11, sizeof(cmd11) },
    { cmd12, sizeof(cmd12) },
    { cmd13, sizeof(cmd13) },
    { cmd14, sizeof(cmd14) },
    { cmd15, sizeof(cmd15) },
    { cmd16, sizeof(cmd16) },
    { cmd17, sizeof(cmd17) },
    { cmd18, sizeof(cmd18) },
    { cmd19, sizeof(cmd19) },
    { cmd20, sizeof(cmd20) },
    { cmd21, sizeof(cmd21) },
    { cmd22, sizeof(cmd22) },
    { cmd23, sizeof(cmd23) },
    { cmd24, sizeof(cmd24) },
    { cmd25, sizeof(cmd25) },
    { cmd26, sizeof(cmd26) },
    { cmd27, sizeof(cmd27) },
    { cmd28, sizeof(cmd28) },
    { cmd29, sizeof(cmd29) },
    { cmd30, sizeof(cmd30) },
    { cmd31, sizeof(cmd31) },
    { cmd32, sizeof(cmd32) },
    { cmd33, sizeof(cmd33) },
    { cmd34, sizeof(cmd34) },
    { cmd35, sizeof(cmd35) },
    { cmd36, sizeof(cmd36) },
    { cmd37, sizeof(cmd37) },
    { cmd38, sizeof(cmd38) },
    { cmd39, sizeof(cmd39) },
    { cmd40, sizeof(cmd40) },
    { cmd41, sizeof(cmd41) },
    { cmd42, sizeof(cmd42) },
    { cmd43, sizeof(cmd43) },
    { cmd44, sizeof(cmd44) },
    { cmd45, sizeof(cmd45) },
    { cmd46, sizeof(cmd46) },
};
static const size_t init_cmds_len = sizeof(init_cmds) / sizeof(init_cmds[0]);

static void init_sensor(libusb_device_handle *dev) {
    int r;

    // 1) control 0xC3 초기 설정
    unsigned char c3_data[16] = {
        0x80, 0x84, 0x1e, 0x00,
        0x08, 0x00, 0x00, 0x01,
        0x01, 0x01, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00
    };

    r = libusb_control_transfer(
        dev,
        0x40,        // Host->Device, Vendor, Device
        0xC3,
        0x0000,
        0x0000,
        c3_data,
        sizeof(c3_data),
        500
    );

    if (r < 0)
        die("control 0xC3 전송 실패", r);

    // 2) 0xA9/0xA8 init 시퀀스
    for (size_t i = 0; i < init_cmds_len; i++) {
        const unsigned char *cmd = init_cmds[i].data;
        size_t len = init_cmds[i].len;

        int transferred = 0;
        r = libusb_bulk_transfer(
            dev,
            BULK_EP_OUT,
            (unsigned char *)cmd,
            (int)len,
            &transferred,
            500
        );
        if (r < 0) {
            fprintf(stderr, "[-] init bulk 전송 실패 idx=%zu, err=%d\n", i, r);
            die("init 실패", r);
        }
    }
}

/* full frame 캡처: 기존 코드 그대로 */
static int capture_frame(libusb_device_handle *dev, unsigned char **out_buf, int *out_len) {
    int r;
    int transferred;
    int total_len = 0;
    int capacity = (int)CAPTURE_NUM_CHUNKS * BULK_PACKET_SIZE + 1024;
    unsigned char *buf = malloc(capacity);

    if (!buf)
        die("malloc 실패", -1);

    // ---------- 1) 첫 chunk: 상태 응답만 처리 (데이터 누적 X) ----------
    {
        uint16_t wIndex0 = capture_indices[0];

        // CONTROL 0xCA (첫 chunk)
        r = libusb_control_transfer(
            dev,
            0x40,
            0xCA,
            0x0003,
            wIndex0,
            NULL,
            0,
            500
        );
        if (r < 0) {
            fprintf(stderr, "[-] control 0xCA 실패 chunk=0, err=%d\n", r);
            goto out_fail;
        }

        // 캡처 시작 명령 (a8 06 00 00 ...)
        unsigned char start_cmd[256];
        memset(start_cmd, 0, sizeof(start_cmd));
        start_cmd[0] = 0xa8;
        start_cmd[1] = 0x06;
        start_cmd[2] = 0x00;
        start_cmd[3] = 0x00;

        r = libusb_bulk_transfer(
            dev,
            BULK_EP_OUT,
            start_cmd,
            sizeof(start_cmd),
            &transferred,
            500
        );

        if (r < 0) {
            fprintf(stderr, "[-] 캡처 시작 bulk 전송 실패 (chunk=0, err=%d)\n", r);
            goto out_fail;
        }

        // 첫 IN: 짧은 상태 응답만 읽고 버림 (0~2 bytes)
        unsigned char tmp0[256];
        r = libusb_bulk_transfer(
            dev,
            BULK_EP_IN,
            tmp0,
            sizeof(tmp0),
            &transferred,
            500
        );
        if (r < 0) {
            fprintf(stderr, "[-] 초기 상태 bulk IN 실패 chunk=0, err=%d\n", r);
            goto out_fail;
        }
        // printf("[*] 초기 상태 응답: %d bytes\n", transferred);
    }

    // ---------- 2) 나머지 chunk: 실제 데이터 + ACK ----------
    for (size_t i = 1; i < CAPTURE_NUM_CHUNKS; i++) {
        uint16_t wIndex = capture_indices[i];

        // CONTROL 0xCA: 청크 설정
        r = libusb_control_transfer(
            dev,
            0x40,
            0xCA,
            0x0003,
            wIndex,
            NULL,
            0,
            500
        );
        if (r < 0) {
            fprintf(stderr, "[-] control 0xCA 실패 chunk=%zu, err=%d\n", i, r);
            break;
        }

        // 데이터 IN (256 bytes 기대)
        unsigned char tmp[256];
        r = libusb_bulk_transfer(
            dev,
            BULK_EP_IN,
            tmp,
            sizeof(tmp),
            &transferred,
            1000
        );

        if (r < 0) {
            fprintf(stderr, "[-] bulk IN 실패 chunk=%zu, err=%d\n", i, r);
            break;
        }

        if (transferred == 0) {
            fprintf(stderr, "[*] chunk=%zu 에서 0 bytes 들어옴, 종료함\n", i);
            break;
        }

        if (total_len + transferred > capacity) {
            capacity *= 2;
            buf = realloc(buf, capacity);
            if (!buf)
                die("realloc(die)", -1);
        }
        memcpy(buf + total_len, tmp, transferred);
        total_len += transferred;

        // ACK (256 zeros)
        unsigned char ack[256] = {0};
        r = libusb_bulk_transfer(
            dev,
            BULK_EP_OUT,
            ack,
            sizeof(ack),
            &transferred,
            500
        );

        if (r < 0) {
            fprintf(stderr, "[-] bulk ACK 실패 chunk=%zu, err=%d\n", i, r);
            break;
        }
    }

    *out_buf = buf;
    *out_len = total_len;
    return 0;

out_fail:
    free(buf);
    *out_buf = NULL;
    *out_len = 0;
    return -1;
}

/* ---- 여기부터 finger detect 추가 ---- */

/* detect용 짧은 캡처: 에러 나면 -1, 성공이면 0 */
static int detect_probe(libusb_device_handle *dev, unsigned char **out_buf, int *out_len, int max_chunks) {
    int r;
    int transferred;
    int total_len = 0;
    int capacity = max_chunks * BULK_PACKET_SIZE + 256;
    unsigned char *buf = malloc(capacity);

    if (!buf)
        die("detect malloc fail", -1);

    // chunk 0: 상태 응답만 (capture_frame과 동일 패턴)
    {
        uint16_t wIndex0 = capture_indices[0];

        r = libusb_control_transfer(
            dev,
            0x40,
            0xCA,
            0x0003,
            wIndex0,
            NULL,
            0,
            500
        );
        if (r < 0) {
            fprintf(stderr, "[-] detect: control 0xCA 실패 chunk=0, err=%d\n", r);
            goto out_fail;
        }

        unsigned char start_cmd[256];
        memset(start_cmd, 0, sizeof(start_cmd));
        start_cmd[0] = 0xa8;
        start_cmd[1] = 0x06;
        start_cmd[2] = 0x00;
        start_cmd[3] = 0x00;

        r = libusb_bulk_transfer(
            dev,
            BULK_EP_OUT,
            start_cmd,
            sizeof(start_cmd),
            &transferred,
            500
        );
        if (r < 0) {
            // fprintf(stderr, "[-] detect: 캡처 시작 bulk 전송 실패 chunk=0, err=%d\n", r);
            goto out_fail;
        }

        unsigned char tmp0[256];
        r = libusb_bulk_transfer(
            dev,
            BULK_EP_IN,
            tmp0,
            sizeof(tmp0),
            &transferred,
            500
        );
        if (r < 0) {
            fprintf(stderr, "[-] detect: 초기 상태 bulk IN 실패 chunk=0, err=%d\n", r);
            goto out_fail;
        }

        if (transferred > 0) {
            if (total_len + transferred > capacity) {
                capacity *= 2;
                buf = realloc(buf, capacity);
                if (!buf)
                    die("detect realloc 실패", -1);
            }
            memcpy(buf + total_len, tmp0, transferred);
            total_len += transferred;
        }
    }

    // chunk 1..max_chunks-1: 일부 데이터만 읽고 ACK
    for (int i = 1; i < max_chunks; i++) {
        uint16_t wIndex = capture_indices[i];

        r = libusb_control_transfer(
            dev,
            0x40,
            0xCA,
            0x0003,
            wIndex,
            NULL,
            0,
            500
        );
        if (r < 0) {
            fprintf(stderr, "[-] detect: control 0xCA 실패 chunk=%d, err=%d\n", i, r);
            break;
        }

        unsigned char tmp[256];
        r = libusb_bulk_transfer(
            dev,
            BULK_EP_IN,
            tmp,
            sizeof(tmp),
            &transferred,
            700
        );
        if (r < 0) {
            fprintf(stderr, "[-] detect: bulk IN 실패 chunk=%d, err=%d\n", i, r);
            break;
        }
        if (transferred == 0) {
            fprintf(stderr, "[*] detect: chunk=%d 에서 0 bytes, 종료\n", i);
            break;
        }

        if (total_len + transferred > capacity) {
            capacity *= 2;
            buf = realloc(buf, capacity);
            if (!buf)
                die("detect realloc 실패", -1);
        }
        memcpy(buf + total_len, tmp, transferred);
        total_len += transferred;

        unsigned char ack[256] = {0};
        r = libusb_bulk_transfer(
            dev,
            BULK_EP_OUT,
            ack,
            sizeof(ack),
            &transferred,
            500
        );
        if (r < 0) {
            fprintf(stderr, "[-] detect: bulk ACK 실패 chunk=%d, err=%d\n", i, r);
            break;
        }
    }

    *out_buf = buf;
    *out_len = total_len;
    return 0;

out_fail:
    free(buf);
    *out_buf = NULL;
    *out_len = 0;
    return -1;
}

/* python _has_finger_in_detect 기반 heuristic */
static int has_finger_in_detect(const unsigned char *data, int len) {
    if (!data || len < 512) {
        fprintf(stderr, "[detect] data too short for finger detect (len=%d)\n", len);
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
    // 25.12.07 잠깐멈춤
    printf("%d\n", total);

    double zero_ratio = (double)zeros / (double)total;
    double ff_ratio = (double)ff / (double)total;

    if (ff_ratio > 0.30 && zeros < total * 0.95) {
        fprintf(stderr,
                "[+] detect figner stats: total=%d, zeros=%d (%.2f), ff=%d (%.2f)\n",
                total, zeros, zero_ratio, ff, ff_ratio);
        return 1;
    }

    return 0;
}

/* python wait_finger 비슷한 루프 (단, C에선 open/close 재사용 안 함) */
static int wait_finger(libusb_device_handle *dev) {
    const int max_loop = 10;
    const int probes_per_loop = 10;

    for (int loop = 0; loop < max_loop; loop++) {
        // printf("[*] finger detect loop %d/%d...\n", loop + 1, max_loop);

        for (int i = 0; i < probes_per_loop; i++) {
            unsigned char *buf = NULL;
            int len = 0;
            int r = detect_probe(dev, &buf, &len, 6);
            if (r == 0 && buf) {
                int finger = has_finger_in_detect(buf, len);
                free(buf);

                if (finger) 
                    return 1;
            }

            struct timespec ts = {0, 100 * 1000 * 1000}; // 100ms
            nanosleep(&ts, NULL);
        }

        // printf("[*] loop wait_finger\n");
        init_sensor(dev);
    }

    printf("[-] finger detect timeout\n");
    return 0;
}

int main(int argc, char** argv) {
    int r;
    libusb_device_handle *dev = NULL;

    printf("========================================\n");
    printf("       samsung 730b libusb test\n");
    printf("========================================\n\n");
    
    printf("[*] 센서 초기화 중...\n");
    r = libusb_init(NULL);
    if (r < 0)
        die("libusb_init 실패", r);

    dev = libusb_open_device_with_vid_pid(NULL, SAMSUNG730B_VID, SAMSUNG730B_PID);
    if (!dev)
        die("장치를 찾을 수 없음 (VID/PID or sudo..?)", -1);

    if (libusb_kernel_driver_active(dev, 0) == 1) {
        r = libusb_detach_kernel_driver(dev, 0);
        if (r < 0)
            die("커널 드라이버 분리 실패", r);
    }

    r = libusb_set_configuration(dev, 1);
    if (r < 0)
        die("set_configuration 실패", r);

    r = libusb_claim_interface(dev, 0);
    if (r < 0)
        die("claim_interface 실패", r);

    init_sensor(dev);
    printf("[+] 센서 초기화 완료\n");

    printf("[*] 손가락을 센서위에 올려놓으세요...\n\12");
    if (!wait_finger(dev)) {
        libusb_release_interface(dev, 0);
        libusb_close(dev);
        libusb_exit(NULL);
        return 1;
    }
    init_sensor(dev);

    unsigned char *buf = NULL;
    int len = 0;
    r = capture_frame(dev, &buf, &len);
    if (r < 0 || !buf)
        die("캡처 실패", r);

    int non_zero = 0;
    for (int i = 0; i < len; i++)
        if (buf[i] != 0) non_zero++;

    printf("[+] 지문캡처 완료: %d bytes, non-zero=%d bytes\n", len, non_zero);

    const char *fname = "capture.raw";
    FILE *f = fopen(fname, "wb");
    if (!f)
        die("capture.raw 열기 실패", -1);
    fwrite(buf, 1, len, f);
    fclose(f);
    printf("[+] RAW 저장됨: %s\n", fname);
    save_pgm_from_raw(buf, len, "capture.pgm", 1);

    free(buf);

    libusb_release_interface(dev, 0);
    libusb_close(dev);
    libusb_exit(NULL);

    printf("[+] 프로그램 종료\n\12");
    return 0;
}