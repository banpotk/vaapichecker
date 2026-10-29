# AppImage: Debian 13 และ Ubuntu 24.04

สคริปต์สร้าง **AppImage ไฟล์เดียว** สำหรับ Debian 13 และ Ubuntu 24.04 ใช้ Ubuntu 24.04 เป็นฐาน compile เพื่อไม่ให้ executable ต้องการ glibc ใหม่กว่าระบบเป้าหมาย รองรับ build บน `x86_64` และ `aarch64` ตาม architecture ของเครื่องที่ build ไม่มี cross-compilation

## วิธีที่แนะนำ: build ใน container

ติดตั้ง Podman หรือ Docker และให้บัญชีปัจจุบันเรียกใช้งาน engine นั้นได้ ไม่ต้องติดตั้ง Qt development packages บน host จากนั้นรันที่ root ของโปรเจกต์:

```bash
./scripts/build-appimage.sh
```

เลือก engine หรือจำนวนงาน compile ได้:

```bash
./scripts/build-appimage.sh --engine docker --jobs 4
./scripts/build-appimage.sh --engine podman --jobs 2
```

ลำดับการทำงาน:

1. สร้าง Ubuntu 24.04 builder image พร้อม C++20, Qt6 และ libva development files
2. ดาวน์โหลด linuxdeploy, Qt deployment plugin, appimagetool และ AppImage runtime จาก fixed releases ของ upstream แล้วตรวจ SHA256 ตาม `tools.lock`
3. Build แบบ Release และรัน QtTest; ติดตั้งลง AppDir พร้อม desktop entry และ SVG icon
4. รวม Qt6 libraries, SVG support, XCB / Wayland / offscreen plugins และ dependencies ที่เหมาะกับการ bundle
5. สร้าง AppImage แล้วทดสอบ startup, GUI และ JSON report ของแพ็กเกจ
6. ทดสอบ **AppImage ไฟล์เดียวกัน** ใน Ubuntu 24.04 และ Debian 13 containers ที่ไม่มี system Qt6 ทั้ง offscreen, X11 ผ่าน Xvfb และ Wayland ผ่าน headless Weston

ไฟล์ผลลัพธ์:

```text
dist/VAAPI_Checker-0.1.0-x86_64.AppImage
dist/VAAPI_Checker-0.1.0-x86_64.AppImage.sha256
dist/VAAPI_Checker-0.1.0-x86_64.AppImage.build-info.txt
dist/VAAPI_Checker-0.1.0-x86_64.ubuntu-24.04.smoke-test.log
dist/VAAPI_Checker-0.1.0-x86_64.debian-13-slim.smoke-test.log
```

ชื่อ version อ่านจาก CMake และ architecture จะเปลี่ยนเป็น `aarch64` เมื่อ build บน ARM64 โดยต้องตรวจบนเครื่อง architecture นั้นด้วย

เก็บ build/download cache ใน `build-appimage/` และ output ใน `dist/` ซึ่งถูก git ignore ไว้ Source mounts เป็น read-only และ container build เขียนผลลัพธ์ด้วย UID/GID ของผู้ใช้; ไม่ mount home directory หรือขอ privileged container

หากไม่มี `tests/test_checker.cpp` เช่นโฟลเดอร์ `tests/` ถูกลบ สคริปต์จะแจ้งเตือนและข้าม QtTest อัตโนมัติ ไม่ mount โฟลเดอร์ที่ไม่มีอยู่ แต่ยังรัน package smoke tests และ compatibility checks ตามปกติ เมื่อกู้ไฟล์ทดสอบกลับมา การ build ครั้งถัดไปจะรัน QtTest อีกครั้งโดยไม่ต้องล้าง cache ใช้ `--skip-tests` เพื่อข้าม QtTest เองได้เช่นกัน

ครั้งแรกต้องเชื่อมต่ออินเทอร์เน็ตสำหรับ container images, apt packages และ packaging tools ครั้งต่อไปใช้ tool/compiler cache เดิมได้ Tool releases ถูก pin ไว้ แต่ apt package versions และ base image security updates ยังขึ้นกับวันที่ build; `build-info.txt` บันทึก versions ที่ใช้งานจริง

## Native build บน Ubuntu 24.04

หากต้องการ build โดยไม่มี container ให้ใช้ **Ubuntu 24.04** และติดตั้ง:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build pkg-config curl ca-certificates \
  file patchelf squashfs-tools python3 desktop-file-utils \
  qt6-base-dev qt6-base-dev-tools qt6-wayland libqt6svg6-dev \
  libva-dev fonts-dejavu-core

./scripts/build-appimage.sh --native --skip-compat-check
```

ยังรัน QtTest และ package smoke tests ในเครื่อง build อยู่ `--skip-compat-check` ข้ามเฉพาะการทดสอบสองดิสโทรผ่าน container หากมี Podman/Docker ใช้ `--native` โดยไม่ข้ามขั้นตอนนี้ได้

Native build บน Debian 13 ถูกปฏิเสธเพื่อรักษา Ubuntu 24.04 baseline ให้ใช้ container mode เมื่อ build จาก Debian 13 หรือดิสโทรอื่น

## เปิด AppImage

```bash
chmod +x dist/VAAPI_Checker-0.1.0-x86_64.AppImage
./dist/VAAPI_Checker-0.1.0-x86_64.AppImage
```

หาก FUSE ใช้งานไม่ได้ ใช้ extraction mode ซึ่งไม่ต้อง mount AppImage:

```bash
./dist/VAAPI_Checker-0.1.0-x86_64.AppImage --appimage-extract-and-run
```

Headless report:

```bash
./dist/VAAPI_Checker-0.1.0-x86_64.AppImage --appimage-extract-and-run --report report.json
```

บนเครื่องเป้าหมายใช้ **libva / libdrm / GPU driver จากระบบ** จึงต้องมี runtime และ driver ที่ตรงกับ GPU ของเครื่อง ตัวอย่าง runtime packages สำหรับทั้งสองดิสโทร:

```bash
sudo apt install libva2 libva-drm2 libgl1 libegl1 libopengl0 \
  libwayland-client0 libwayland-cursor0 libwayland-egl1
```

ติดตั้ง VAAPI driver ตาม GPU และสิทธิ์เข้าถึง `/dev/dri/renderD*` ตามปกติ หากต้องใช้ Media Components ให้มี `ffmpeg` และ `ffprobe` ใน PATH ของเครื่องปลายทาง

Qt6 รวมไว้ใน AppImage ส่วน libva, libdrm, OpenGL/EGL, Wayland libraries, libstdc++ และ VAAPI driver ไม่ bundle เพื่อให้ GPU stack ใช้ versions ที่ตรงกันกับระบบ การ bundle Wayland จาก Ubuntu 24.04 จะทำให้ Mesa บน Debian 13 เรียก `wl_display_create_queue_with_name` ไม่ได้ Host FFmpeg ใช้ PATH และ library environment เดิม; AppRun ไม่เปลี่ยน `LD_LIBRARY_PATH` จึงไม่บังคับให้ FFmpeg โหลด Qt หรือ library ที่แพ็กมากับแอพ

## ตรวจแพ็กเกจที่สร้างไว้แล้ว

```bash
./scripts/verify-appimage.sh --engine docker \
  dist/VAAPI_Checker-0.1.0-x86_64.AppImage

cd dist
sha256sum --check VAAPI_Checker-0.1.0-x86_64.AppImage.sha256
```

Compatibility smoke tests ตรวจ version, sample JSON, missing-device error/exit code, GUI offscreen ในโหมด System/Light/Dark, GUI X11 และ GUI Wayland โดยไม่ติดตั้ง Qt6 ใน runtime container การตรวจ GPU จริงยังต้องทำบนเครื่องที่มี GPU/driver และ expose render node; smoke tests ไม่แทนการตรวจ acceleration จริง

ตัวเลือกอื่น:

```bash
./scripts/build-appimage.sh --output-dir ./release-output
./scripts/build-appimage.sh --skip-tests
./scripts/build-appimage.sh --skip-compat-check
./scripts/build-appimage.sh --help
```

หากเปลี่ยน packaging tool release ให้อัปเดต URL และ SHA256 ใน `packaging/appimage/tools.lock` ควบคู่กัน สคริปต์จะหยุดเมื่อ checksum ไม่ตรง

อ้างอิง: [AppImage packaging guide](https://docs.appimage.org/packaging-guide/from-source/native-binaries.html), [AppImage best practices](https://docs.appimage.org/reference/best-practices.html), [linuxdeploy Qt plugin](https://github.com/linuxdeploy/linuxdeploy-plugin-qt)
