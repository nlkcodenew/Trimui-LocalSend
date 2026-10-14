# Trimui-LocalSend

Ứng dụng LocalSend độc lập cho **TrimUI Brick Pro** (1024x768, 4:3) và
**TrimUI Smart Pro S** (1280x720, 16:9). Một binary duy nhất tự nhận diện
độ phân giải màn hình lúc chạy (theo cách của chiaki-ng) và co giãn giao
diện cho vừa. Phần giao thức được tách từ module LocalSend của
`bun2it/RomCloud` tại commit `80635bf`, sau đó loại bỏ toàn bộ phụ thuộc
Google Drive, database ROM, OTA và media player.

## Chức năng

- Tự tìm thiết bị LocalSend trong cùng mạng Wi-Fi bằng UDP multicast/broadcast.
- Gửi một file bất kỳ từ thẻ nhớ tới điện thoại hoặc máy tính.
- Hiện hộp thoại chấp nhận/từ chối file đến (hỗ trợ nhiều file / cả thư mục trong 1 lần gửi).
- Lưu file nhận vào `/mnt/SDCARD/Download/LocalSend` mà không tự giải nén hoặc di chuyển.
- Hiện tiến trình gửi và nhận trực tiếp trên màn hình (tự thích ứng 1024x768 và 1280x720).

## Điều khiển

- `D-Pad`: di chuyển.
- `A`: mở thư mục, chọn file hoặc chấp nhận file đến.
- `B`: quay lại, từ chối hoặc thoát ứng dụng.
- `X`: mở trình chọn file; trong màn tiến trình, xóa tác vụ đã hoàn tất.
- `Y`: quét lại thiết bị.
- `START`: mở màn tiến trình.

## Build

Trong WSL/Linux với TG5050 SDK ở `~/tb/sdk`:

```sh
./build.sh
./package.sh
```

Gói cài đặt được tạo tại `dist/Trimui-LocalSend-v0.2.0.zip` (một gói duy nhất
dùng chung cho cả Brick Pro và Smart Pro S).

## Cài đặt

Giải nén ZIP vào thư mục gốc của thẻ nhớ. Kết quả phải là:

```text
/mnt/SDCARD/Apps/Trimui-LocalSend/
```

Khởi động lại MainUI hoặc thiết bị, rồi mở `Trimui-LocalSend` trong mục Apps.
Có thể tải bản mới nhất tại mục Releases của
https://github.com/nlkcodenew/Trimui-LocalSend.

## Cập nhật tự động (OTA)

Từ v0.3.0, app tự cập nhật **im lặng** theo mô hình của LottoForecast-Pro —
người dùng không cần biết, không cần bấm gì:

- `launch.sh` chạy `ota-update.sh --apply` nền: 30 giây sau khi mở app kiểm
  tra 1 lần, sau đó mỗi 10 phút kiểm tra lại trong khi app còn mở.
- Script tải `ota/manifest.json` từ GitHub (raw.githubusercontent, dự phòng
  jsdelivr), **binary verify chữ ký Ed25519 của manifest trước** khi tin bất
  kỳ nội dung nào (`--verify-manifest`, dùng OpenSSL; private key ký release
  giữ offline trên máy dev, binary chỉ nhúng public key).
- Từng file được tải về và verify SHA-256 trước khi chép; chép atomic
  (`.ota-new` → rename); không bao giờ đụng tới `data/` (alias, fingerprint),
  `*.log`, `.ota-status`.
- Bản mới có hiệu lực ở lần mở app tiếp theo — không restart giữa chừng để
  không giết transfer đang chạy.
- Tắt hẳn OTA: đặt `TRIMUI_LS_NO_OTA=1`.

Quy trình phát hành bản mới trên máy dev:

```sh
# 1. build + đóng gói như thường
./build.sh && ./package.sh
# 2. chép file release vào ota/files/vX.Y.Z/
# 3. ký manifest
python3 tools/make_manifest.py --version X.Y.Z --files ota/files/vX.Y.Z \
    --key ~/.config/trimui-localsend/ota/ls1.key --kid ls1 \
    --out ota/manifest.json
# 4. commit + push (kèm GitHub release chứa zip như mọi khi)
```

## Tương thích

Ứng dụng triển khai LocalSend protocol v2 qua HTTP trên cổng `53317`. Thiết bị
gửi và nhận phải ở cùng LAN; AP isolation hoặc firewall có thể chặn discovery.
