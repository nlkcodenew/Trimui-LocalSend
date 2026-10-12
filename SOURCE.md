# Nguồn và phạm vi tách

- Upstream: `https://github.com/bun2it/RomCloud`
- Commit tham chiếu: `80635bf5bbc87b07b1aad51e95c87ae46ca1ecaa`
- Module gốc: `src/localsend/`
- Giao thức: LocalSend protocol v2, `https://github.com/localsend/protocol`

Project này giữ phần discovery và transfer của LocalSend, đồng thời thay toàn bộ
lớp cấu hình, logging, giao diện và đóng gói bằng implementation độc lập dành
cho TrimUI Stock OS. Không chứa Google Drive, database ROM, OTA, IPTV, YouTube,
GameCast hoặc các module khác của RomCloud.
