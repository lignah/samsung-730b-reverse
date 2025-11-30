# samsung-730b-reverse
리눅스에서 Samsung 730b 지문인식을 위한 리버싱 노트, python/libusb 드라이버 테스트 (libfprint 기여용)

목적 :

    삼성 노트북에서 리눅스 사용시 지문인식 안됨
    그래서 되게 하려 함

시작 :

    의존성: pyusb, Pillow

    사용법:

    pip install pyusb pillow
    sudo python scripts/samsung_730b_driver_v1_1.py --debug

지금까지 파악된 상태 :

    지문캡쳐 잘 됨 (offset 182, 112x96, 90도 회전)
    finger detect : 리버싱 진행중, 아직 미사용

s730b_test.c 빌드 :

    sudo pacman -S libusb
    ls /usr/include/libusb-1.0/libusb.h
    #include <libusb-1.0/libusb.h>
    gcc -Wall -O2 s730b_test.c -o s730b_test -lusb-1.0