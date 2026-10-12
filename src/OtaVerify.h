#pragma once

#include <string>

// Verify manifest OTA ký Ed25519 (theo mô hình LottoForecast-Pro).
// Manifest do tools/make_manifest.py sinh và ký bằng private key giữ offline;
// binary chỉ nhúng public key. Trả true khi chữ ký hợp lệ, false kèm lỗi
// ghi ra stderr.
bool OtaVerifyManifest(const std::string &manifestPath);

// Public key release hiện tại (kid -> base64url của 32 byte raw).
// Thêm khóa dự phòng ở đây khi xoay khóa, client chọn theo `kid` trong manifest.
struct OtaReleaseKey {
  const char *kid;
  const char *pubB64Url;
};
const OtaReleaseKey *OtaReleaseKeys();
int OtaReleaseKeyCount();
