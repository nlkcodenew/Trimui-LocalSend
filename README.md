# Trimui-LocalSend

Ứng dụng LocalSend độc lập cho TrimUI Brick Pro chạy Stock OS. Phần giao thức
được tách từ module LocalSend của `bun2it/RomCloud` tại commit `80635bf`, sau đó
loại bỏ toàn bộ phụ thuộc Google Drive, database ROM, OTA và media player.

## Chức năng

- Tự tìm thiết bị LocalSend trong cùng mạng Wi-Fi bằng UDP multicast/broadcast.
- Gửi một file bất kỳ từ thẻ nhớ tới điện thoại hoặc máy tính.
- Hiện hộp thoại chấp nhận/từ chối file đến.
- Lưu file nhận vào `/mnt/SDCARD/Download/LocalSend` mà không tự giải nén hoặc di chuyển.
- Hiện tiến trình gửi và nhận trực tiếp trên màn hình 1024x768.

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

Gói cài đặt được tạo tại `dist/Trimui-LocalSend-stock-v0.1.0.zip`.

## Cài đặt

Giải nén ZIP vào thư mục gốc của thẻ nhớ. Kết quả phải là:

```text
/mnt/SDCARD/Apps/Trimui-LocalSend/
```

Khởi động lại MainUI hoặc thiết bị, rồi mở `Trimui-LocalSend` trong mục Apps.

## Tương thích

Ứng dụng triển khai LocalSend protocol v2 qua HTTP trên cổng `53317`. Thiết bị
gửi và nhận phải ở cùng LAN; AP isolation hoặc firewall có thể chặn discovery.
