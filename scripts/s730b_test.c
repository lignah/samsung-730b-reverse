/*
 * s730b_test.c
 *
 * - Samsung 730B를 libusb로 잡아서 캡처 시도하는 실험용 코드
 * - 현재(25.11.30) 기준:
 *   - 센서 초기화(init_sensor)는 python driver v1.1과 동일하게 동작하는 것 같음
 *   - 캡처(capture_frame)는 chunk=0에서 bulk ACK 타임아웃 나면서
 *     전체 길이가 2~3 bytes 수준으로만 들어오는 WIP 상태임

[*] 캡처 시작...
[-] bulk ACK 실패 chunk=0, err=-7
[+] 캡처 완료: 2 bytes, non-zero=0 bytes
[+] RAW 저장됨: capture.raw
[+] s730b_test 종료

 * - 원인?:
 *   - Windows/pylibusb 시퀀스와 libusb C 시퀀스의 미세한 차이(패킷크기나 타이밍)가
 *     남아있어서 센서가 캡처 모드로 안 들어간 듯
 * - TODO:
 *   - python driver캡처 시 usbmon으로 패킷 덤프
 *   - 동일 조건에서 s730b_test실행 후 usbmon 덤프
 *   - 두 pcap 비교해서 control 0xCA / bulk OUT(a8 06..) / bulk IN/ACK 패턴을
 *     1:1로 맞춘 뒤 다시 테스트 해봐야할듯
 */